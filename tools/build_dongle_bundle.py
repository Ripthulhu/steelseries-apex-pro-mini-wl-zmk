#!/usr/bin/env python3
"""Build the receiver, recovery bootloader and stock USB installer bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from build_dongle import tool

ROOT = Path(__file__).resolve().parents[1]


def run(*args, env=None):
    subprocess.run([str(a) for a in args], check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    workspace, output = args.workspace.resolve(), args.output.resolve()
    venv = workspace / '.venv'
    python = venv / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
    if Path(sys.prefix).resolve() != venv.resolve():
        return subprocess.call([str(python), str(Path(__file__).resolve()), *sys.argv[1:]])
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    recipe = [ROOT / 'dongle/bootloader/port.patch',
              *sorted((ROOT / 'bootloader/apex_dongle_wl').glob('*'))]
    if hashlib.sha256(recipe[0].read_bytes().replace(b'\r\n', b'\n')).hexdigest() != lock['patches']['dongle/bootloader/port.patch']:
        raise RuntimeError('Dongle bootloader patch differs from dependencies.lock.json')
    digest = hashlib.sha256(b''.join(p.read_bytes().replace(b'\r\n', b'\n')
                                     for p in recipe if p.is_file())).hexdigest()[:12]
    source = output / ('boot-source-' + digest)
    marker = source / '.apex-dongle-recipe'
    if source.exists() and not marker.is_file():
        raise RuntimeError(f'Incomplete bootloader checkout: {source}')
    if not source.exists():
        base = workspace.parent / lock['repositories']['adafruit_nrf52_bootloader']['path']
        revision = lock['repositories']['adafruit_nrf52_bootloader']['revision']
        source.parent.mkdir(parents=True, exist_ok=True)
        run('git', '-C', base, 'worktree', 'add', '--detach', source, revision)
        shutil.copytree(ROOT / 'bootloader/apex_dongle_wl', source / 'src/boards/apex_dongle_wl')
        run('git', '-C', source, 'apply', '--ignore-space-change', recipe[0])
        run('git', '-C', source, 'submodule', 'update', '--init',
            'lib/nrfx', 'lib/tinycrypt', 'lib/tinyusb', 'lib/uf2')
        marker.write_text(digest + '\n')
    build = source / '_build_dongle'
    env = os.environ.copy()
    env['SOURCE_DATE_EPOCH'] = str(lock['source_date_epoch'])
    run(tool('cmake'), '-S', source, '-B', build, '-GNinja',
        '-DBOARD=apex_dongle_wl', '-DSD_VERSION=7.2.0', '-DDONGLE_BUILD_ID=' + digest,
        '-DCMAKE_MAKE_PROGRAM=' + tool('ninja'),
        '-DZEPHYR_SDK_INSTALL_DIR=' + (workspace / '.zephyr-sdk').as_posix(),
        '-DPython_EXECUTABLE=' + sys.executable, env=env)
    run(tool('cmake'), '--build', build, '--target', 'bootloader', env=env)
    run(sys.executable, ROOT / 'tools/build_dongle.py', '--workspace', workspace,
        '--output', output / 'receiver', '--radio-input')
    artifacts = sorted((output / 'receiver/artifacts').glob('*/build.json'),
                       key=lambda p: p.stat().st_mtime)[-1].parent
    bundle = output / 'apex-dongle'
    bundle.mkdir(exist_ok=True)
    for name in ('apex-receiver.uf2', 'apex-receiver.hex', 'apex-receiver.elf',
                 'apex-receiver.config', 'build.json'):
        shutil.copy2(artifacts / name, bundle / name)
    for name in ('bootloader.elf', 'bootloader_mbr.hex', 'bootloader_mbr.uf2'):
        shutil.copy2(build / name, bundle / name)
    run(sys.executable, ROOT / 'dongle/installer/build.py', '--workspace', workspace,
        '--boot', build / 'bootloader_mbr.hex', '--output', bundle)
    for name in ('install.py', 'usb_update.py', 'readback.py'):
        shutil.copy2(ROOT / 'dongle/installer' / name, bundle / name)
    shutil.copy2(ROOT / 'tools/dongle.py', bundle / 'dongle.py')
    shutil.copy2(ROOT / 'dongle/INSTALL.md', bundle / 'INSTALL.md')
    shutil.copy2(ROOT / 'dongle/installer/70-apex-dongle.rules', bundle / '70-apex-dongle.rules')
    licenses = {
        ROOT / 'LICENSE': 'LICENSE.txt',
        ROOT / 'THIRD_PARTY_NOTICES.md': 'THIRD_PARTY_NOTICES.txt',
        workspace / 'zephyr/LICENSE': 'LICENSE-ZEPHYR.txt',
        workspace / 'modules/lib/picolibc/COPYING.picolibc': 'LICENSE-PICOLIBC.txt',
        workspace / 'modules/lib/picolibc/COPYING.NEWLIB': 'LICENSE-NEWLIB.txt',
        source / 'LICENSE': 'LICENSE-ADAFRUIT-BOOTLOADER.txt',
        source / 'lib/tinyusb/LICENSE': 'LICENSE-TINYUSB.txt',
        source / 'lib/uf2/LICENSE.txt': 'LICENSE-UF2.txt',
        source / 'lib/nrfx/LICENSE': 'LICENSE-NRFX.txt',
        source / 'lib/tinycrypt/LICENSE': 'LICENSE-TINYCRYPT.txt',
        source / 'lib/softdevice/mbr/hex/mbr_nrf52_2.4.1_licence-agreement.txt': 'LICENSE-NORDIC-MBR.txt',
        source / 'lib/softdevice/s140_nrf52_7.2.0/s140_nrf52_7.2.0_licence-agreement.txt': 'LICENSE-NORDIC-S140.txt',
    }
    for original, name in licenses.items():
        shutil.copy2(original, bundle / name)
    sums = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(bundle.iterdir()) if p.is_file() and p.name != 'SHA256SUMS.json'}
    (bundle / 'SHA256SUMS.json').write_text(json.dumps(sums, indent=2) + '\n')
    archive = output / 'apex-dongle.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zipped:
        for p in sorted(bundle.iterdir()):
            if p.is_file():
                zipped.write(p, p.name)
    print('Receiver installer bundle:', archive)


if __name__ == '__main__':
    sys.exit(main())
