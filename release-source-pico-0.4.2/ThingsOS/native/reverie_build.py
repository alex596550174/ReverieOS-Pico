# SPDX-License-Identifier: Apache-2.0
"""Build the ReverieOS rescue and main application with an activated ESP-IDF."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from pico_release import BUILD, NATIVE, RESCUE, release_configuration


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--print-version', action='store_true')
    args = parser.parse_args()
    config = release_configuration()
    if args.print_version:
        print(config['version'])
        return

    idf_path = os.environ.get('IDF_PATH')
    if not idf_path or not (Path(idf_path) / 'tools/idf.py').is_file():
        parser.error('Activate the pinned ESP-IDF environment before building.')
    idf = Path(idf_path).resolve()
    revision = subprocess.check_output(['git', '-C', str(idf), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != config['esp_idf_commit']:
        parser.error(f'Expected ESP-IDF {config["esp_idf_commit"]}, found {revision}.')

    env = {**os.environ, 'IDF_TARGET': 'esp32s3'}
    BUILD.mkdir(parents=True, exist_ok=True)
    for script in ('prepare_dependencies.py', 'prepare_assets.py'):
        subprocess.run([sys.executable, str(NATIVE / script)], env=env, check=True)
    for project, output, parameters in [
        ('rescue', RESCUE, []),
        ('firmware', BUILD, [f'-DTHINGSOS_BUILD_VERSION={config["version"]}']),
    ]:
        subprocess.run([sys.executable, str(idf / 'tools/idf.py'),
                        '-C', str(NATIVE / project), '-B', str(output),
                        *parameters, 'build'], env=env, check=True)


if __name__ == '__main__':
    main()
