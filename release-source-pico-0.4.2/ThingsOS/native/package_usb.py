"""Build a complete local USB installation from hash-verified validation inputs.

No serial connection, publication or device acceptance is implied by packaging.
The image initializes NVS/OTA slots; SD books, fonts and wallpapers are separate.
"""
from pathlib import Path
import hashlib
import json
import sys

from pico_release import (BUILD, checked_images, merge_images, ota_selector, read_json,
                          release_configuration, require, validation_records)

OUT = BUILD.parent / 'native-usb'


def package():
    config = release_configuration()
    version = config['version']
    validation = read_json(BUILD / 'validation.json')
    require(validation['build_dependency_boundary_passed'] and validation['boot_guard_linked'],
            'Re-run verify_build.py before packaging.')
    require(validation['version'] == version and validation['usb_flash_enabled']
            and '100% tests passed' in validation['host_tests'],
            'Version or validation changed. Re-run verify_build.py before packaging.')

    images = checked_images(config)
    require(validation['images'] == validation_records(images), 'Build inputs changed. Re-run verify_build.py.')
    merged = merge_images(images)
    OUT.mkdir(parents=True, exist_ok=True)
    image = OUT / f'ReverieOS-{version}-usb.bin'
    image.write_bytes(merged)

    main = images['app'][3]
    app = OUT / f'ReverieOS-{version}-ota.bin'
    app.write_bytes(main)
    manifest = {'version': version, 'chip': 'ESP32-S3', 'offset': 0,
                'file': image.name, 'bytes': len(merged), 'sha256': hashlib.sha256(merged).hexdigest(),
                'ota': {'file': app.name, 'bytes': len(main), 'sha256': hashlib.sha256(main).hexdigest()},
                'initializes_settings_and_ota': True, 'sd_untouched': True,
                'device_validation': 'not performed for this build', 'published': False}
    web = {'name': 'ReverieOS', 'version': version,
           'builds': [{'chipFamily': 'ESP32-S3', 'parts': [{'path': image.name, 'offset': 0}]}]}
    for name, value in [('manifest.json', manifest), ('install-manifest.json', web)]:
        (OUT / name).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--ota-selector':
        Path(sys.argv[2]).write_bytes(ota_selector())
    elif len(sys.argv) == 1:
        package()
    else:
        raise SystemExit('Usage: package_usb.py [--ota-selector PATH]')
