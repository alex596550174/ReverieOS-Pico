#!/bin/bash
set -eo pipefail
umask 077

usage() {
    cat <<'HELP'
ReverieOS Read Pico · macOS USB

  check                          读取芯片、Flash 与分区表
  backup                         备份整片 16 MiB 并校验 SHA-256
  flash-app [app.bin]            增量刷写 ThingsOS ota_0
  restore-app <ota_0_original.bin>  写回本机校验过的原始 ota_0 备份

可用 PICO_PORT 指定 /dev/cu.* 端口，PICO_BAUD 默认 460800。
备份路径为 ~/ThingsOS-Pico-Backups/。原厂分区不能用本脚本增量刷写。
HELP
}

die() { printf '错误：%s\n' "$*" >&2; exit 1; }

command_name=${1:-help}
case "$command_name" in
    help|-h|--help) usage; exit 0 ;;
    check|backup) [ "$#" -eq 1 ] || die "$command_name 无需其他参数" ;;
    flash-app) [ "$#" -le 2 ] || die '用法：flash-app [app.bin]' ;;
    restore-app) [ "$#" -eq 2 ] && [ -n "$2" ] || die '用法：restore-app <ota_0_original.bin>' ;;
    *) usage >&2; die "未知子命令 ${command_name}" ;;
esac

build_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)/build
flasher="$build_dir/native-idf/flasher_args.json"
backup_root="$HOME/ThingsOS-Pico-Backups"
if [[ "$command_name" = flash-app || "$command_name" = restore-app ]]; then
    [ -d "$backup_root" ] || die '没有整片备份，请先运行 backup'
fi

sdk="$HOME/esp/esp-idf"
[ -r "$sdk/export.sh" ] || die "未找到 ${sdk}/export.sh"
# IDF export.sh uses optional environment variables without nounset guards.
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH"
source "$sdk/export.sh" >/dev/null
set -u
export LC_ALL=C

pico_data() {
    python3 - "$@" <<'PY'
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

FLASH = 0x1000000
OTA = 0x220000
OTA_SIZE = 0xae0000
PARTITION = 0x8000
SECTOR = 0x1000
EXPECTED = {
    'nvs': (1, 2, 0x9000, 0x6000, 0),
    'otadata': (1, 0, 0xf000, 0x2000, 0),
    'phy_init': (1, 1, 0x11000, 0x1000, 0),
    'rescue': (0, 0, 0x20000, 0x200000, 0),
    'ota_0': (0, 0x10, OTA, OTA_SIZE, 0),
    'system_font': (1, 0x40, 0xd00000, 0x300000, 0),
}
BACKUP_FILES = ('flash_id.txt', 'full_16mb.bin', 'nvs_backup.bin', 'ota_0_original.bin')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def partition_table(sector):
    if len(sector) != SECTOR:
        raise ValueError('分区表扇区必须为 4096 字节')
    rows = {}
    ranges = []
    md5_valid = False
    for index in range(0, 0xc00, 32):
        entry = sector[index:index+32]
        if entry == b'\xff' * 32:
            break
        if entry[:2] == b'\xeb\xeb':
            if entry[2:16] != b'\xff' * 14 or entry[16:] != hashlib.md5(sector[:index]).digest():
                raise ValueError('分区表 MD5 错误')
            md5_valid = True
            index += 32
            break
        magic, kind, subtype, offset, size, label, flags = struct.unpack('<HBBII16sI', entry)
        label = label.split(b'\0', 1)[0].decode('ascii')
        if (magic != 0x50aa or not label or label in rows or not size or offset < 0x9000
                or offset % SECTOR or size % SECTOR or offset + size > FLASH
                or (kind == 0 and offset % 0x10000)):
            raise ValueError(f'分区条目无效，位置 {index:#x}')
        rows[label] = kind, subtype, offset, size, flags
        ranges.append((offset, offset + size))
    else:
        raise ValueError('分区表缺少结束记录')
    if not rows or any(value != 0xff for value in sector[index:0xc00]):
        raise ValueError('分区表结束记录无效')
    ranges.sort()
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise ValueError('分区表内存在重叠分区')
    return md5_valid and rows == EXPECTED, rows


def mac_from(info):
    if not re.search(r'(?im)Detected flash size:\s*16\s*MB\b', info):
        raise ValueError('Flash ID 未确认 16 MiB')
    matches = set(m.lower() for m in re.findall(r'(?im)\bMAC:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})\b(?!:)', info))
    if len(matches) != 1:
        raise ValueError('无法确认设备 MAC')
    return matches.pop()


def load_full(path):
    if path.stat().st_size != FLASH:
        raise ValueError('整片备份长度必须为 16 MiB')
    return path.read_bytes()


def backup_contents(folder, expected_mac, create=False):
    full = load_full(folder / 'full_16mb.bin')
    matched, _ = partition_table(full[PARTITION:PARTITION+SECTOR])
    info = (folder / 'flash_id.txt').read_bytes()
    if mac_from(info.decode('utf-8')) != expected_mac:
        raise ValueError('备份设备 MAC 不匹配')
    parts = {'nvs_backup.bin': full[0x9000:0xf000], 'ota_0_original.bin': full[OTA:OTA+OTA_SIZE]}
    for name, content in parts.items():
        path = folder / name
        if create:
            path.write_bytes(content)
        elif path.stat().st_size != len(content) or path.read_bytes() != content:
            raise ValueError(f'{name} 与整片备份的对应区间不一致')
    contents = {'flash_id.txt': info, 'full_16mb.bin': full, **parts}
    sums = ''.join(f'{digest(contents[name])}  {name}\n' for name in BACKUP_FILES)
    manifest = folder / 'SHA256SUMS.txt'
    if create:
        manifest.write_text(sums, encoding='ascii')
    elif manifest.read_text(encoding='ascii') != sums:
        raise ValueError('备份 SHA-256 校验失败')
    return matched, digest(parts['ota_0_original.bin'])


def flash_config(path):
    config = json.loads(path.read_text(encoding='utf-8'))
    app = config['app']
    relative = app['file']
    settings = config['flash_settings']
    root = path.resolve().parent
    if (config['extra_esptool_args']['chip'] != 'esp32s3' or config['extra_esptool_args']['stub'] is not True
            or int(app['offset'], 0) != OTA or config['flash_files'].get('0x220000') != relative
            or app.get('encrypted') not in (None, False, 'false')):
        raise ValueError('flasher_args.json 未确认 ESP32-S3 明文 ota_0 和 stub 模式')
    mode, freq, size = (settings[key] for key in ('flash_mode', 'flash_freq', 'flash_size'))
    if mode not in ('qio', 'qout', 'dio', 'dout') or not re.fullmatch(r'(20|26|40|80|120)m', freq) or size != '16MB':
        raise ValueError('flasher_args.json 的 Flash 参数不符合本机型')
    image = (root / relative).resolve()
    if not image.is_relative_to(root) or '\n' in str(image):
        raise ValueError('构建镜像路径无效')
    print(mode, freq, size, image, sep='\n')


def verify_app(path):
    if not 0 < path.stat().st_size < OTA_SIZE:
        raise ValueError('应用镜像必须非空且严格小于 ota_0（0xae0000 字节）')
    with path.open('rb') as stream:
        header = stream.read(112)
    if (len(header) < 112 or header[0] != 0xe9 or struct.unpack_from('<H', header, 12)[0] != 9
            or struct.unpack_from('<I', header, 32)[0] != 0xabcd5432
            or header[80:112].split(b'\0', 1)[0] != b'ThingsOS_native'):
        raise ValueError('文件不是 ESP32-S3 的 ThingsOS 主应用镜像')


try:
    action, *args = sys.argv[1:]
    if action == 'config':
        flash_config(Path(args[0]))
    elif action == 'app':
        verify_app(Path(args[0]))
    elif action == 'restore':
        if Path(args[0]).stat().st_size != OTA_SIZE:
            raise ValueError('回退文件必须是完整的 ota_0_original.bin（0xae0000 字节）')
    elif action == 'identity':
        print(mac_from(Path(args[0]).read_text(encoding='utf-8')))
    elif action == 'check':
        matched, rows = partition_table(Path(args[0]).read_bytes())
        for name, (_, _, start, length, _) in rows.items():
            print(f'{name:12} @ 0x{start:06x} (0x{length:06x})')
        if not matched:
            raise ValueError('分区表不匹配（如仍为原厂固件），严禁直接 flash-app')
        print('已匹配 ThingsOS 分区，可直接走 ota_0 增量刷写')
    elif action == 'save':
        folder = Path(args[0])
        matched, _ = backup_contents(folder, args[1], create=True)
        backup_contents(folder, args[1])
        print('整片备份、切片与 SHA-256 已校验。')
        if not matched:
            print('备份来自非 ThingsOS 分区布局。ota_0 切片仅保留原始字节，不能用 restore-app 写回。')
    elif action == 'backup-match':
        root, device_mac, *restore = args
        target = None
        if restore:
            path = Path(restore[0])
            if path.stat().st_size != OTA_SIZE:
                raise ValueError('回退文件必须是完整的 ota_0_original.bin（0xae0000 字节）')
            target = digest(path.read_bytes())
        for folder in sorted(Path(root).iterdir(), reverse=True):
            if not folder.is_dir() or not re.fullmatch(r'\d{8}-\d{6}-[A-Za-z0-9]+', folder.name):
                continue
            try:
                matched, ota_hash = backup_contents(folder, device_mac)
            except (OSError, ValueError):
                continue
            if target is None or (matched and target == ota_hash):
                print(f'整片备份校验通过：{folder}')
                break
        else:
            raise ValueError('没有符合当前设备且哈希有效的整片备份，请先运行 backup')
except (OSError, ValueError, KeyError, TypeError) as error:
    sys.exit(f'错误：{error}')
PY
}

shopt -s nullglob
if [ -n "${PICO_PORT:-}" ]; then
    port=$PICO_PORT
    [[ "$port" == /dev/cu.* ]] && [ -c "$port" ] || die "无效串口 ${port}"
else
    ports=(/dev/cu.usbmodem* /dev/cu.usbserial*)
    case "${#ports[@]}" in
        0) die '未找到 USB 串口，请连接 Read Pico 或指定 PICO_PORT' ;;
        1) port=${ports[0]}; [ -c "$port" ] || die "无效串口 ${port}" ;;
        *) printf '检测到多个候选串口，请明确指定 PICO_PORT：\n' >&2
           printf '  %s\n' "${ports[@]}" >&2; exit 1 ;;
    esac
fi
baud=${PICO_BAUD:-460800}
[[ "$baud" =~ ^[1-9][0-9]{3,6}$ ]] || die "无效波特率 ${baud}"
work=$(mktemp -d "${TMPDIR:-/tmp}/thingsos-pico.XXXXXXXX")
backup_work=
cleanup() {
    rm -rf -- "$work"
    if [ -n "$backup_work" ]; then rm -rf -- "$backup_work"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

esptool_device() {
    python3 -m esptool --chip esp32s3 --port "$port" --baud "$baud" "$@"
}

identify() {
    esptool_device --after no-reset flash-id | tee "$1"
    esptool_device --after no-reset read-mac | tee -a "$1"
    pico_data identity "$1" > "$work/mac"
}

check_partition() {
    esptool_device --after no-reset read-flash 0x8000 0x1000 "$work/partitions.bin"
    pico_data check "$work/partitions.bin" || die '无法确认 ThingsOS 分区，禁止增量写入'
}

case "$command_name" in
    check)
        identify "$work/flash_id.txt"
        check_partition
        ;;
    backup)
        mkdir -p -- "$backup_root"
        # Publish the timestamped directory only after verification succeeds.
        stamp=$(date '+%Y%m%d-%H%M%S')
        backup_work=$(mktemp -d "$backup_root/.${stamp}.XXXXXXXX")
        identify "$backup_work/flash_id.txt"
        esptool_device --after no-reset read-flash 0x000000 0x1000000 "$backup_work/full_16mb.bin"
        pico_data save "$backup_work" "$(cat "$work/mac")"
        final="$backup_root/${stamp}-${backup_work##*.}"
        mv -- "$backup_work" "$final"
        backup_work=
        printf '备份目录：%s\n' "$final"
        ;;
    flash-app|restore-app)
        pico_data config "$flasher" > "$work/config"
        { IFS= read -r mode; IFS= read -r freq; IFS= read -r size; IFS= read -r default_app; } < "$work/config"
        image=${2:-$default_app}
        [ -f "$image" ] || die "找不到镜像 ${image}"
        if [ "$command_name" = flash-app ]; then
            pico_data app "$image"
        else
            pico_data restore "$image"
        fi
        cp -- "$image" "$work/app.bin"
        if [ "$command_name" = flash-app ]; then
            pico_data app "$work/app.bin"
            python3 -m esptool --chip esp32s3 image-info "$work/app.bin" > "$work/image-info.txt"
            grep -Eq '^Checksum: 0x[0-9a-f]+ \(valid\)$' "$work/image-info.txt" &&
                grep -Eq '^Validation hash: [0-9a-f]{64} \(valid\)$' "$work/image-info.txt" ||
                die '应用镜像的校验和或内置 SHA-256 无效'
        fi
        identify "$work/flash_id.txt"
        if [ "$command_name" = flash-app ]; then
            pico_data backup-match "$backup_root" "$(cat "$work/mac")"
        else
            pico_data backup-match "$backup_root" "$(cat "$work/mac")" "$work/app.bin"
        fi
        check_partition
        esptool_device write-flash --flash-mode "$mode" --flash-freq "$freq" --flash-size "$size" 0x220000 "$work/app.bin"
        printf '写入完成。唯一写入区间为 ota_0。\n'
        ;;
esac
