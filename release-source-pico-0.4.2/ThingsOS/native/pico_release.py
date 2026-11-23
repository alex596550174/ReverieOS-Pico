# SPDX-License-Identifier: Apache-2.0
"""Read Pico release configuration and checks shared by validation and packaging."""
import binascii
import hashlib
import json
from pathlib import Path
import re
import struct

NATIVE = Path(__file__).resolve().parent
SNAPSHOT = NATIVE.parents[1]
BUILD = NATIVE.parent / 'build/native-idf'
RESCUE = NATIVE.parent / 'build/native-rescue'

# ESP32-S3 Read Pico RDP-G01-W. Keep these checks independent of the input CSV
# so a stale or accidentally changed table cannot redefine the install layout.
FLASH_CAPACITY = 0x1000000
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0x1000
# Name: type, subtype, address, capacity, flags
PARTITIONS = {
    'nvs': (1, 2, 0x9000, 0x6000, 0),
    'otadata': (1, 0, 0xf000, 0x2000, 0),
    'phy_init': (1, 1, 0x11000, 0x1000, 0),
    'rescue': (0, 0, 0x20000, 0x200000, 0),
    'ota_0': (0, 0x10, 0x220000, 0xae0000, 0),
    'system_font': (1, 0x40, 0xd00000, 0x300000, 0),
}
# Flash argument key, file relative to the main build, address, capacity.
FLASH_FILES = (
    ('bootloader', 'bootloader/bootloader.bin', 0, TABLE_OFFSET),
    ('partition-table', 'partition_table/partition-table.bin', TABLE_OFFSET, TABLE_SIZE),
    ('otadata', 'ota_data_usb.bin', *PARTITIONS['otadata'][2:4]),
    ('rescue', '../native-rescue/ThingsOS_rescue.bin', *PARTITIONS['rescue'][2:4]),
    ('app', 'ThingsOS_native.bin', *PARTITIONS['ota_0'][2:4]),
    ('system-font', 'system_font.bin', *PARTITIONS['system_font'][2:4]),
)
LOAD_ENTRY = struct.Struct('<HBBII16sI')
TABLE_DIGEST_HEADER = b'\xeb\xeb' + b'\xff' * 14
ERASED_ENTRY = b'\xff' * LOAD_ENTRY.size


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def release_configuration():
    fork = read_json(NATIVE / 'reverie-release.json')
    snapshot = read_json(SNAPSHOT / 'SOURCE-RELEASE.json')
    rescue_version = snapshot['version']
    require(re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', rescue_version), 'Invalid rescue version.')
    version = fork['version']
    suffix = version.removeprefix(rescue_version)
    require(version.startswith(rescue_version) and len(version) <= 31
            and re.fullmatch(r'-R-[0-9]+(?:\.[0-9]+\.[0-9]+)?', suffix),
            f'Expected {rescue_version}-R-N or {rescue_version}-R-N.N.N within 31 ASCII bytes.')
    require(re.fullmatch(r'[0-9a-f]{40}', fork['source_snapshot_commit']), 'Invalid snapshot source commit.')
    return {**fork, 'rescue_version': rescue_version, 'esp_idf_commit': snapshot['esp_idf_commit']}


def check_partition_table(data):
    require(0 < len(data) <= TABLE_SIZE and len(data) % LOAD_ENTRY.size == 0,
            'Invalid Read Pico partition table size.')
    partitions = {}
    for index in range(0, len(data), LOAD_ENTRY.size):
        entry = data[index:index + LOAD_ENTRY.size]
        if entry.startswith(TABLE_DIGEST_HEADER):
            digest = hashlib.md5(data[:index], usedforsecurity=False).digest()
            require(entry[16:] == digest, 'Read Pico partition table MD5 mismatch.')
            tail = data[index + LOAD_ENTRY.size:]
            require(tail.startswith(ERASED_ENTRY) and tail == b'\xff' * len(tail),
                    'Partition data after the MD5 record must be erased.')
            require(partitions == PARTITIONS, 'Read Pico partition names, types, addresses, capacities or flags differ.')
            return

        magic, kind, subtype, address, capacity, name_bytes, flags = LOAD_ENTRY.unpack(entry)
        require(magic == 0x50aa, 'Invalid or missing Read Pico partition record.')
        name = name_bytes.rstrip(b'\0').decode('ascii')
        require(name not in partitions, f'Duplicate Read Pico partition {name!r}.')
        partitions[name] = (kind, subtype, address, capacity, flags)
    raise ValueError('The Read Pico partition table is missing its MD5 record.')


def ota_selector():
    selector = bytearray(b'\xff' * PARTITIONS['otadata'][3])
    crc = binascii.crc32(struct.pack('<I', 1), 0xffffffff) & 0xffffffff
    require(crc == 0x4743989a, 'Unexpected OTA selector CRC.')
    selector[:32] = struct.pack('<I20sII', 1, b'\xff' * 20, 0, crc)
    return bytes(selector)


def check_flasher_args(args):
    expected = {offset: path for _, path, offset, _ in FLASH_FILES}
    files = args['flash_files']
    require(len(files) == len(expected) and {int(key, 0): value for key, value in files.items()} == expected,
            'Flash image paths or addresses differ from the Read Pico layout.')
    for key, path, offset, _ in FLASH_FILES:
        item = args[key]
        require((item['file'], int(item['offset'], 0), item['encrypted']) == (path, offset, 'false'),
                f'Unexpected flash arguments for {key}.')
    settings = {'flash_mode': 'dio', 'flash_size': '16MB', 'flash_freq': '80m'}
    require(args['flash_settings'] == settings and args['extra_esptool_args']['chip'] == 'esp32s3',
            'Read Pico requires an ESP32-S3 with DIO, 16 MB flash and 80 MHz.')
    options = args['write_flash_args']
    require(len(options) == 6 and dict(zip(options[::2], options[1::2])) == {
        '--flash-mode': 'dio', '--flash-size': '16MB', '--flash-freq': '80m',
    }, 'Flash command options differ from the verified Read Pico settings.')


def check_image(data, name, version=None):
    require(len(data) >= 24 and data[0] == 0xe9 and struct.unpack_from('<H', data, 12)[0] == 9,
            f'{name} is not an ESP32-S3 image.')
    require(data[2:4] == b'\x02\x4f', f'{name} has unexpected flash settings.')
    if version is not None:
        require(len(data) >= 288 and data[32:36] == b'\x32\x54\xcd\xab',
                f'{name} does not have an ESP application descriptor.')
        require(data[80:112] == name.encode('ascii').ljust(32, b'\0'), f'Unexpected project ID for {name}.')
        require(data[48:80] == version.encode('ascii').ljust(32, b'\0'), f'{name} must have version {version}.')


def checked_images(config):
    check_flasher_args(read_json(BUILD / 'flasher_args.json'))
    images = {}
    for key, relative, offset, capacity in FLASH_FILES:
        path = (BUILD / relative).resolve()
        data = path.read_bytes()
        require(0 < len(data) <= capacity, f'Image size exceeds its reserved flash region: {key}.')
        images[key] = (path, offset, capacity, data)

    table = images['partition-table'][3]
    check_partition_table(table)
    rescue_table = (RESCUE / 'partition_table/partition-table.bin').read_bytes()
    require(table == rescue_table, 'Main and rescue partition tables differ.')
    require(images['otadata'][3] == ota_selector(), 'Read Pico OTA boot selector changed.')
    check_image(images['bootloader'][3], 'bootloader')
    check_image(images['app'][3], 'ThingsOS_native', config['version'])
    check_image(images['rescue'][3], 'ThingsOS_rescue', config['rescue_version'])
    return images


def merge_images(images):
    parts = sorted((offset, data) for _, offset, _, data in images.values())
    end = 0
    for offset, data in parts:
        require(offset >= end and offset + len(data) <= FLASH_CAPACITY, 'Flash image regions overlap or exceed 16 MiB.')
        end = offset + len(data)

    merged = bytearray(b'\xff' * end)
    for offset, data in parts:
        merged[offset:offset + len(data)] = data
    return merged


def image_record(image):
    path, offset, capacity, data = image
    return {'file': str(path), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'development_offset': hex(offset), 'capacity': capacity}


def validation_records(images):
    # Keep the main application first for historical consumers of validation.json.
    order = ('app', 'rescue', 'system-font', 'bootloader', 'partition-table', 'otadata')
    return [image_record(images[key]) for key in order]
