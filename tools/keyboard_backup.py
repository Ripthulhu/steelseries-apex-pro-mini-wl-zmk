"""Read the keyboard's complete flash through its wired Zephyr shell."""
import hashlib
import json
from pathlib import Path
import re
import time

import serial


def read_chunk(port, device, offset, size=1024):
    port.write(f'flash read {device}{offset:x} {size:x}\r'.encode('ascii'))
    lines = {}
    pending = b''
    deadline = time.monotonic() + 15
    while len(lines) < size // 16 and time.monotonic() < deadline:
        pending += port.read(min(port.in_waiting, 4096) or 1)
        complete = pending.split(b'\n')
        pending = complete.pop()
        if len(pending) > 4096:
            raise RuntimeError('Unexpected flash response')
        for line in complete:
            match = re.search(rb'([0-9a-fA-F]{8}): ([0-9a-fA-F ]+)\|', line)
            if not match:
                continue
            address = int(match[1], 16)
            data = bytes.fromhex(match[2].decode('ascii'))
            if offset <= address < offset + size and address % 16 == 0 and len(data) == 16:
                if address in lines and lines[address] != data:
                    raise RuntimeError('Conflicting flash read-back')
                lines[address] = data
    if len(lines) != size // 16:
        raise RuntimeError(f'Incomplete flash read at 0x{offset:x}; backup is not usable')
    return b''.join(lines[a] for a in range(offset, offset + size, 16))


def backup_keyboard(port_name, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    manifest = {}
    print('Reading flash over USB. This can take about 25 minutes. Keep the keyboard connected.', flush=True)
    with serial.Serial(port_name, 115200, timeout=.1, write_timeout=2) as port:
        port.dtr = True
        port.write(b'\x03\r')
        time.sleep(.3)
        port.reset_input_buffer()
        for name, device, total in (('internal.bin', '', 0x80000),
                                    ('external.bin', 'ext_flash@0 ', 0x100000)):
            digest = hashlib.sha256()
            with (directory / name).open('xb') as output:
                for offset in range(0, total, 1024):
                    chunk = read_chunk(port, device, offset)
                    output.write(chunk)
                    digest.update(chunk)
                    if (offset + 1024) % 65536 == 0:
                        print(f'{name}: {offset + 1024}/{total}', flush=True)
            manifest[name] = {'size': total, 'sha256': digest.hexdigest()}
    with (directory / 'manifest.json').open('x', encoding='ascii') as output:
        json.dump(manifest, output, indent=2)
        output.write('\n')
    print(f'Backup complete: {directory.resolve()}. Keep it private; it contains pairing keys.')
