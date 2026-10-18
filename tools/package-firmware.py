#!/usr/bin/env python3
"""Merge the tested PlatformIO images into a single ESP32-S3 image at offset 0."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--core-dir', type=Path, default=Path(os.environ.get('PLATFORMIO_CORE_DIR', str(Path.home() / '.platformio'))))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
built = repo / '.pio/build/freenove_fnk0104b'
packages = args.core_dir / 'packages'
esptool = packages / 'tool-esptoolpy/esptool.py'
boot_app = packages / 'framework-arduinoespressif32/tools/partitions/boot_app0.bin'
output = repo / 'build/tpdd2-r0.1-fnk0104b.bin'
for source in [esptool, boot_app, built / 'bootloader.bin', built / 'partitions.bin', built / 'firmware.bin']:
    if not source.is_file():
        parser.error(f'Missing {source}; build firmware first and check --core-dir')
output.parent.mkdir(exist_ok=True)
subprocess.run([sys.executable, str(esptool), '--chip', 'esp32s3', 'merge_bin',
                '-o', str(output), '--flash_size', '16MB',
                '0x0', str(built / 'bootloader.bin'),
                '0x8000', str(built / 'partitions.bin'),
                '0xe000', str(boot_app), '0x10000', str(built / 'firmware.bin')], check=True)
digest = hashlib.sha256(output.read_bytes()).hexdigest()
output.with_suffix('.bin.sha256').write_text(f'{digest}  {output.name}\n')
print(f'{output}\nSHA256 {digest}')
