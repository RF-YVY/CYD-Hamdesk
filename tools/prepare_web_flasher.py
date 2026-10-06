"""Package clean build outputs for Pages; never read a connected device's flash."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def prepare(version: str) -> None:
    root = Path(__file__).resolve().parents[1]
    build = root / '.pio/build/cyd35'
    web = root / 'web'
    output = web / 'firmware'
    output.mkdir(parents=True, exist_ok=True)
    boot_app = Path.home() / '.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
    inputs = [build / 'bootloader.bin', build / 'partitions.bin', boot_app, build / 'firmware.bin']
    for path in inputs:
        if not path.is_file():
            raise FileNotFoundError(f'Build first: missing {path}')
    # The default 4MB min_spiffs layout puts the first application at 0x10000.
    partitions = inputs[1].read_bytes()
    app = None
    for start in range(0, len(partitions), 32):
        entry = partitions[start:start + 32]
        if entry[:2] == b'\xaa\x50' and entry[2] == 0:
            app = (int.from_bytes(entry[4:8], 'little'), int.from_bytes(entry[8:12], 'little'))
            break
    if app is None or app[0] != 0x10000 or inputs[-1].stat().st_size > app[1]:
        raise ValueError('Unexpected application partition or oversized firmware')
    merged = output / 'hamdesk-e32r35t-install.bin'
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32', 'merge-bin',
                    '--output', str(merged), '--flash-mode', 'dio', '--flash-freq', '40m',
                    '--flash-size', '4MB',
                    *[item for offset, path in zip(('0x1000', '0x8000', '0xe000', '0x10000'), inputs)
                      for item in (offset, str(path))]], check=True)
    update = output / 'hamdesk-e32r35t-update.bin'
    shutil.copyfile(inputs[-1], update)
    # Verify image composition, including the application bytes, before publishing.
    image = merged.read_bytes()
    for offset, source in zip((0x8000, 0xe000, 0x10000), inputs[1:]):
        data = source.read_bytes()
        if image[offset:offset + len(data)] != data:
            raise ValueError(f'Merged image mismatch at {offset:#x}')
    for mode, path, offset, erase_prompt in (
            ('install', merged, 0, False), ('update', update, 0x10000, True)):
        manifest = {'name': 'Ham Desk E32R35T', 'version': version,
                    'new_install_prompt_erase': erase_prompt, 'new_install_improv_wait_time': 0,
                    'builds': [{'chipFamily': 'ESP32', 'parts': [
                        {'path': f'firmware/{path.name}', 'offset': offset}]}]}
        (web / f'manifest-{mode}.json').write_text(json.dumps(manifest, indent=2) + '\n')
    hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (merged, update)}
    (output / 'SHA256SUMS.txt').write_text(''.join(f'{value}  {name}\n' for name, value in hashes.items()))
    (web / 'build.json').write_text(json.dumps({'version': version, 'board': 'E32R35T', 'sha256': hashes}, indent=2) + '\n')
    print(f'Prepared {version}: verified installation and update images')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    prepare(parser.parse_args().version)
