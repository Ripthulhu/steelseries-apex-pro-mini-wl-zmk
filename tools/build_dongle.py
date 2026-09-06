#!/usr/bin/env python3
"""Build receiver firmware using the project's existing pinned ZMK workspace."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def tool(name):
    suffix = '.exe' if os.name == 'nt' else ''
    beside_python = Path(sys.executable).parent / (name + suffix)
    result = beside_python if beside_python.is_file() else shutil.which(name)
    if not result:
        raise RuntimeError(f'{name} not found; use the environment from tools/setup_workspace.py')
    return str(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    variant = parser.add_mutually_exclusive_group()
    variant.add_argument('--radio-probe', action='store_true', help='enable the 10 Hz fixed-channel hardware test')
    variant.add_argument('--radio-input', action='store_true', help='enable the keyboard/media receiver with channel hopping')
    args = parser.parse_args()
    workspace, output = args.workspace.resolve(), args.output.resolve()
    venv = workspace / '.venv'
    python = venv / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
    if not python.is_file():
        raise RuntimeError('Build environment missing; run python tools/setup_workspace.py first')
    if Path(sys.prefix).resolve() != venv.resolve():
        # Preserve the venv interpreter path: resolving its symlink loses the environment.
        return subprocess.call([str(python), str(Path(__file__).resolve()), *sys.argv[1:]])
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    revisions = {'zephyr': lock['repositories']['zephyr']['revision'],
                 'modules/hal/nordic': lock['repositories']['hal_nordic']['revision'],
                 'modules/hal/cmsis': lock['west_revisions']['modules/hal/cmsis'],
                 'modules/crypto/tinycrypt': lock['west_revisions']['modules/crypto/tinycrypt']}
    dependency_patches = {}
    for name, revision in revisions.items():
        path = workspace / name
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise RuntimeError(f'{name} does not match dependencies.lock.json')
        dirty = subprocess.check_output(['git', '-C', str(path), 'diff', '--binary', 'HEAD', '--'])
        if dirty:
            dependency_patches[name] = dirty
    inputs = {}
    for folder in ('dongle', 'radio'):
        for p in sorted((ROOT / folder).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts:
                inputs[p.relative_to(ROOT).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    inputs['dependencies.lock.json'] = hashlib.sha256((ROOT / 'dependencies.lock.json').read_bytes()).hexdigest()
    inputs['tools/build_dongle.py'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    inputs['variant'] = 'radio-input' if args.radio_input else 'radio-probe' if args.radio_probe else 'usb-console'
    for name, patch in dependency_patches.items():
        inputs['dependency-patch:' + name] = hashlib.sha256(patch).hexdigest()
    source_id = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()[:12]
    output.mkdir(parents=True, exist_ok=True)
    build = output / 'build'
    env = os.environ.copy()
    env['ZEPHYR_BASE'] = (workspace / 'zephyr').as_posix()
    env['ZEPHYR_SDK_INSTALL_DIR'] = (workspace / '.zephyr-sdk').as_posix()
    env['ZEPHYR_TOOLCHAIN_VARIANT'] = 'zephyr'
    env['SOURCE_DATE_EPOCH'] = str(lock['source_date_epoch'])
    commands = [
        [tool('cmake'), '-S', str(ROOT / 'dongle'), '-B', str(build), '-GNinja',
         '-DBOARD=apex_receiver/nrf52833', '-DCMAKE_MAKE_PROGRAM=' + tool('ninja'),
         '-DPython3_EXECUTABLE=' + sys.executable, '-DAPEX_RECEIVER_BUILD_ID=' + source_id,
         '-DEXTRA_CONF_FILE=' + ((ROOT / 'dongle/radio_input.conf').as_posix() if args.radio_input else
                                (ROOT / 'dongle/radio_probe.conf').as_posix() if args.radio_probe else '')],
        [tool('cmake'), '--build', str(build)],
    ]
    with (output / 'build.log').open('w', encoding='utf-8') as log:
        for command in commands:
            subprocess.run(command, env=env, check=True, stdout=log, stderr=subprocess.STDOUT)
    artifacts = output / 'artifacts' / source_id
    artifacts.mkdir(parents=True, exist_ok=True)
    for name, patch in dependency_patches.items():
        (artifacts / (name.replace('/', '-') + '.patch')).write_bytes(patch)
    shutil.copy2(output / 'build.log', artifacts / 'build.log')
    from intelhex import IntelHex
    image = IntelHex(str(build / 'zephyr/zephyr.hex'))
    if not all(0x1000 <= a < b <= 0x6d000 for a, b in image.segments()):
        raise RuntimeError('Receiver image exceeds its application partition')
    data = image.tobinarray(start=0x1000).tobytes()
    count = (len(data) + 255) // 256
    uf2 = bytearray()
    for i in range(count):
        block = bytearray(512)
        struct.pack_into('<8I', block, 0, 0x0a324655, 0x9e5d5157, 0x2000,
                         0x1000 + i * 256, 256, i, count, 0x1d506170)
        block[32:288] = data[i * 256:(i + 1) * 256].ljust(256, b'\xff')
        struct.pack_into('<I', block, 508, 0x0ab16f30)
        uf2 += block
    (artifacts / 'apex-receiver.uf2').write_bytes(uf2)
    for source, target in [('zephyr.hex', 'apex-receiver.hex'), ('zephyr.elf', 'apex-receiver.elf'),
                           ('.config', 'apex-receiver.config')]:
        shutil.copy2(build / 'zephyr' / source, artifacts / target)
    manifest = {'source_id': source_id, 'protocol_version': 1,
                'features': ['usb-shell', 'watchdog', 'packet-crypto-tests', 'usb-pairing-storage'],
                'radio_enabled': args.radio_probe or args.radio_input, 'radio_test_only': args.radio_probe and not args.radio_input,
                'hid_forwarding': args.radio_input,
                'inputs': inputs, 'dependencies': revisions,
                'artifacts': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in artifacts.glob('apex-receiver.*')}}
    (artifacts / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Receiver {source_id}: {len(data)} bytes; artifacts in {artifacts}')


if __name__ == '__main__':
    sys.exit(main())
