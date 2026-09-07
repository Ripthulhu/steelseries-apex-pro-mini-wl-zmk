#!/usr/bin/env python3
"""Install the custom receiver through stock USB, with a two-pass backup first."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import string
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zlib

from usb_update import validate

STOCK_HASH = '7ad309bc6bbb869118937718ee56323ca6158cbb47fa1745a9ed240e66af89fe'
ARCHIVE_HASH = '7c69366aa2b8a1bca301079199537ad7ff990cbd002decf7163910197ed321f9'
ARCHIVE_URL = 'https://engine.steelseriescdn.com/SteelSeriesGG116.0.0Setup.exe'
BOARD = 'Board-ID: nRF52833-ApexProMiniWLDongle-v1'
HERE = Path(__file__).resolve().parent


def check_hash(path, expected):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != expected:
        raise ValueError(f'SHA-256 mismatch: {path}')


def stock_image(cache, supplied=None, seven_zip=None):
    if supplied:
        check_hash(supplied, STOCK_HASH)
        return supplied.read_bytes()
    cache.mkdir(parents=True, exist_ok=True)
    image = cache / 'dongle-3.24.1.bin'
    if image.exists():
        check_hash(image, STOCK_HASH)
        return image.read_bytes()
    extractor = seven_zip or shutil.which('7zz') or shutil.which('7z')
    if not extractor and os.name == 'nt':
        candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / '7-Zip/7z.exe'
        if candidate.is_file():
            extractor = str(candidate)
    if not extractor:
        raise RuntimeError('Install 7-Zip (7zz or 7z), or supply --7zip PATH. GG is not installed or run.')
    archive = cache / 'SteelSeriesGG116.0.0Setup.exe'
    if not archive.exists():
        print('Downloading the official firmware archive (403 MiB). GG will not be run.', flush=True)
        partial = cache / 'download.part'
        request = urllib.request.Request(ARCHIVE_URL, headers={'User-Agent': 'Mozilla/5.0'})
        with urllib.request.urlopen(request, timeout=60) as response, partial.open('wb') as out:
            shutil.copyfileobj(response, out)
        check_hash(partial, ARCHIVE_HASH)
        partial.replace(archive)
    check_hash(archive, ARCHIVE_HASH)
    result = subprocess.run([str(extractor), 'e', '-so', str(archive),
                             'apps/engine/firmware/272111140/firmware-apex-pro-mini-wireless-dongle-3.24.1.bin'],
                            check=True, stdout=subprocess.PIPE)
    if hashlib.sha256(result.stdout).hexdigest() != STOCK_HASH:
        raise ValueError('The extracted receiver firmware does not match version 3.24.1')
    image.write_bytes(result.stdout)
    return result.stdout


def patched_image(stock, patch):
    if hashlib.sha256(stock).hexdigest() != STOCK_HASH:
        raise ValueError('Unsupported stock firmware')
    if patch.get('format') != 1:
        raise ValueError('Unsupported installer patch')
    result = bytearray(stock)
    occupied = set()
    for offset, value in patch['edits']:
        data = bytes.fromhex(value)
        if not data or offset < 0 or offset + len(data) > len(result) - 4:
            raise ValueError('Installer patch outside application')
        positions = set(range(offset, offset + len(data)))
        if occupied & positions:
            raise ValueError('Overlapping installer edits')
        occupied.update(positions)
        result[offset:offset + len(data)] = data
    struct.pack_into('<I', result, len(result) - 4, zlib.crc32(result[:-4]))
    validate(result)
    return result


def check_backup(folder):
    record = json.loads((folder / 'readback.json').read_text())
    for kind, size in (('flash', 0x80000), ('uicr', 0x1000)):
        first, second = [(folder / f'{kind}-{n}.bin').read_bytes() for n in (1, 2)]
        if len(first) != size or first != second or hashlib.sha256(first).hexdigest() != record[kind + '_sha256']:
            raise ValueError('Backup reads do not match: ' + kind)
    uicr = (folder / 'uicr-1.bin').read_bytes()
    if struct.unpack_from('<II', uicr, 0x14) != (0x74000, 0x7e000):
        raise ValueError('Different receiver boot addresses; stopped without changing the bootloader')


def find_drive(explicit=None):
    if explicit:
        candidates = [explicit]
    elif os.name == 'nt':
        candidates = [Path(letter + ':/') for letter in string.ascii_uppercase]
    else:
        candidates = [*Path('/Volumes').glob('*'), *Path('/media').glob('*/*'),
                      *Path('/run/media').glob('*/*'), *Path('/mnt').glob('*')]
    matches = []
    for drive in candidates:
        try:
            if BOARD in (drive / 'INFO_UF2.TXT').read_text():
                matches.append(drive)
        except OSError:
            pass
    if len(matches) > 1:
        raise RuntimeError('Several receiver drives found; connect only one dongle')
    return matches[0] if matches else None


def install_application(image, drive=None, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        destination = find_drive(drive)
        if destination:
            # Recheck the board immediately before opening the destination file.
            if BOARD not in (destination / 'INFO_UF2.TXT').read_text():
                raise RuntimeError('Receiver drive changed')
            with image.open('rb') as src, (destination / 'NEW.UF2').open('wb') as dst:
                shutil.copyfileobj(src, dst)
                dst.flush()
                try:
                    os.fsync(dst.fileno())
                except OSError as error:
                    if error.errno not in (errno.EBADF, errno.EIO, errno.ENODEV, errno.EINVAL):
                        raise
                    # UF2 completion resets the receiver and removes its drive.
                    # main() must still confirm the expected application build.
                    print('Receiver drive disconnected during sync; checking the application next.', flush=True)
            return
        time.sleep(.25)
    raise RuntimeError('APEXDONGLE did not mount. Do not repeat stock installation. Mount it and copy apex-receiver.uf2.')


def validate_application(image):
    data = image.read_bytes()
    if not data or len(data) % 512:
        raise ValueError('Invalid receiver UF2 size')
    count = len(data) // 512
    for index in range(count):
        block = data[index * 512:(index + 1) * 512]
        magic0, magic1, flags, address, size, number, total, family = struct.unpack_from('<8I', block)
        if ((magic0, magic1, flags, size, number, total, family) !=
                (0x0a324655, 0x9e5d5157, 0x2000, 256, index, count, 0x1d506170)
                or address != 0x1000 + 256 * index or address + size > 0x6d000
                or struct.unpack_from('<I', block, 508)[0] != 0x0ab16f30):
            raise ValueError('Wrong board, layout or malformed receiver UF2')


def wait_stock(timeout=30):
    import hid
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        entries = [e for e in hid.enumerate(0x1038, 0) if e['product_id'] in (0x1624, 0x1625)
                   and e['usage_page'] == 0xffc0 and e['usage'] == 1]
        if len(entries) == 1:
            return
        if len(entries) > 1:
            raise RuntimeError('Connect only one Apex receiver')
        time.sleep(.25)
    raise RuntimeError('Stock receiver did not reconnect')


def wait_application(source_id, timeout=30):
    import serial
    from serial.tools import list_ports
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ports = [p for p in list_ports.comports() if (p.vid, p.pid) == (0x1d50, 0x6171)]
        if len(ports) > 1:
            raise RuntimeError('Several custom receivers connected')
        if ports:
            try:
                with serial.Serial(ports[0].device, 115200, timeout=.2, write_timeout=2) as port:
                    port.dtr = True
                    port.write(b'\x03\rdongle status\r')
                    response = port.read(4096)
                    if source_id.encode() in response:
                        return ports[0].device
            except (OSError, serial.SerialException):
                pass
        time.sleep(.5)
    raise RuntimeError('Application build could not be confirmed. Use APEXDONGLE recovery; do not repeat the stock installer.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-image', type=Path, help='optional offline copy of stock 3.24.1 firmware')
    parser.add_argument('--cache', type=Path, default=Path('download-cache'))
    parser.add_argument('--backup', type=Path, default=Path('dongle-backup'))
    parser.add_argument('--7zip', dest='seven_zip', help='7z or 7zz executable')
    parser.add_argument('--drive', type=Path, help='APEXDONGLE mount path if not automatically found')
    parser.add_argument('--prepare-only', action='store_true', help='download and validate files without accessing USB')
    parser.add_argument('--confirm', action='store_true', help='replace the stock receiver firmware and bootloader')
    args = parser.parse_args()
    if not args.prepare_only and not args.confirm:
        parser.error('installation requires --confirm; first read INSTALL.md')
    for name, digest in json.loads((HERE / 'SHA256SUMS.json').read_text()).items():
        if Path(name).name != name:
            raise ValueError('Invalid bundle manifest path')
        check_hash(HERE / name, digest)
    validate_application(HERE / 'apex-receiver.uf2')
    stock = stock_image(args.cache, args.stock_image, args.seven_zip)
    images = {name: patched_image(stock, json.loads((HERE / (name + '.patch.json')).read_text()))
              for name in ('readback', 'install')}
    if args.prepare_only:
        print('Download and installer files verified. No USB access or flashing performed.')
        return
    if args.backup.exists():
        raise RuntimeError('Backup folder already exists; choose a new --backup folder. Existing backups are never overwritten.')
    import hid
    import serial
    args.backup.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryFile(dir=args.backup.parent):
        pass
    wait_stock(timeout=2)
    with tempfile.TemporaryDirectory(prefix='apex-dongle-install-') as folder:
        for name in ('readback', 'install'):
            image = Path(folder) / (name + '.bin')
            image.write_bytes(images[name])
            subprocess.run([sys.executable, str(HERE / 'usb_update.py'), 'flash', str(image),
                            '--sha256', hashlib.sha256(images[name]).hexdigest()], check=True)
            if name == 'readback':
                time.sleep(2)
                wait_stock()
                subprocess.run([sys.executable, str(HERE / 'readback.py'), str(args.backup)], check=True)
                check_backup(args.backup)
                print('Two matching backups saved. Installing the custom bootloader.', flush=True)
        print('Waiting for APEXDONGLE, then installing the receiver application.', flush=True)
        install_application(HERE / 'apex-receiver.uf2', args.drive)
    source_id = json.loads((HERE / 'build.json').read_text())['source_id']
    port = wait_application(source_id)
    print(f'Receiver {source_id} is running on {port}. Keep the backup and pair with the keyboard over USB.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
