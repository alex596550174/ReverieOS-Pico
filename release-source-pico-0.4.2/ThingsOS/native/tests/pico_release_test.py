# SPDX-License-Identifier: Apache-2.0
"""Read Pico release and flash-layout validation with memory-only fixtures."""
import copy
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pico_release import (FLASH_FILES, LOAD_ENTRY, PARTITIONS, TABLE_DIGEST_HEADER,
                          check_flasher_args, check_image, check_partition_table,
                          merge_images, ota_selector, release_configuration)


def partition_table(rows):
    records = b''.join(rows)
    return records + TABLE_DIGEST_HEADER + hashlib.md5(records, usedforsecurity=False).digest() + b'\xff' * 32


class PicoReleaseTest(unittest.TestCase):
    def setUp(self):
        self.rows = [LOAD_ENTRY.pack(0x50aa, kind, subtype, address, capacity, label.encode(), flags)
                     for label, (kind, subtype, address, capacity, flags) in PARTITIONS.items()]

    def test_partition_table_checks_all_fields_even_with_valid_md5(self):
        check_partition_table(partition_table(self.rows))
        for row_number, row in enumerate(self.rows):
            for field in range(1, 7):
                with self.subTest(row=row_number, field=field):
                    fields = list(LOAD_ENTRY.unpack(row))
                    fields[field] = fields[field] ^ 1 if field != 5 else b'wrong-label'
                    rows = self.rows.copy()
                    rows[row_number] = LOAD_ENTRY.pack(*fields)
                    with self.assertRaises(ValueError):
                        check_partition_table(partition_table(rows))

    def test_partition_table_detects_missing_duplicate_and_corrupt_records(self):
        valid = partition_table(self.rows)
        corrupt_digest = bytearray(valid)
        corrupt_digest[len(self.rows) * LOAD_ENTRY.size + 16] ^= 1
        invalid = [valid[:-32], valid[:-1] + b'\0', bytes(corrupt_digest),
                   partition_table(self.rows[:-1]), partition_table(self.rows + [self.rows[0]])]
        for table in invalid:
            with self.assertRaises(ValueError):
                check_partition_table(table)

    def test_flash_commands_check_mapping_and_device(self):
        args = {
            'flash_files': {hex(offset): name for _, name, offset, _ in FLASH_FILES},
            'flash_settings': {'flash_mode': 'dio', 'flash_size': '16MB', 'flash_freq': '80m'},
            'extra_esptool_args': {'chip': 'esp32s3'},
            'write_flash_args': ['--flash-mode', 'dio', '--flash-size', '16MB', '--flash-freq', '80m'],
            **{key: {'file': name, 'offset': hex(offset), 'encrypted': 'false'}
               for key, name, offset, _ in FLASH_FILES},
        }
        check_flasher_args(args)
        for field, bad in [('offset', '0x20000'), ('file', 'wrong.bin'), ('encrypted', 'true')]:
            altered = copy.deepcopy(args)
            altered['app'][field] = bad
            with self.assertRaises(ValueError):
                check_flasher_args(altered)

    def test_main_and_rescue_versions_are_independent(self):
        config = release_configuration()
        image = bytearray(288)
        image[0], image[12] = 0xe9, 9
        image[2:4] = b'\x02\x4f'
        image[32:36] = b'\x32\x54\xcd\xab'
        image[48:80] = config['version'].encode().ljust(32, b'\0')
        image[80:112] = b'ThingsOS_native'.ljust(32, b'\0')
        check_image(image, 'ThingsOS_native', config['version'])
        with self.assertRaises(ValueError):
            check_image(image, 'ThingsOS_native', config['rescue_version'])
        with self.assertRaises(ValueError):
            check_image(image, 'ThingsOS_rescue', config['version'])

    def test_usb_merge_keeps_holes_erased_and_rejects_overlap(self):
        selector = ota_selector()
        self.assertEqual(len(selector), 0x2000)
        self.assertEqual(selector[28:32], b'\x9a\x98\x43\x47')
        self.assertEqual(merge_images({'first': (None, 0, 32, b'a'),
                                       'second': (None, 4, 32, b'b')}), b'a\xff\xff\xffb')
        with self.assertRaises(ValueError):
            merge_images({'first': (None, 0, 32, b'aa'), 'second': (None, 1, 32, b'b')})


if __name__ == '__main__':
    unittest.main()
