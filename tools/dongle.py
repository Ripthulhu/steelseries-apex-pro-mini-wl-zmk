#!/usr/bin/env python3
"""USB maintenance for the custom Apex receiver. Requires pyserial."""
import argparse
import time
import hashlib
import re
import secrets
import struct
import zlib

import serial
from serial.tools import list_ports

BOOT_VID = 0x1D50
BOOT_PID = 0x6170

# Application USB IDs, used to auto-find the shell ports for pairing.
KB_APP = (0x1D50, 0x615E)   # keyboard application
DG_APP = (0x1D50, 0x6171)   # receiver application


class PairClient:
    """Pairing commands on the USB shell. Secret records are never printed."""
    def __init__(self, port, target):
        if target not in ('apex', 'dongle'):
            raise ValueError('Unknown pairing target')
        self.target = target
        self.port = serial.Serial(port, 115200, timeout=0.1, write_timeout=2)
        try:
            self.port.dtr = True
            time.sleep(0.4)
            self.port.write(b'\x03\r')
            time.sleep(0.2)
            self.port.reset_input_buffer()
        except Exception:
            self.port.close()
            raise

    def close(self):
        self.port.close()

    def read_marker(self, expression, timeout=4):
        deadline = time.monotonic() + timeout
        response = b''
        while time.monotonic() < deadline:
            response += self.port.read(1024)
            if len(response) > 16384:
                raise RuntimeError('Unexpected shell output during pairing')
            clean = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', response)
            match = re.search(expression, clean)
            if match:
                return match
            if b'APX_PAIR_ERROR' in clean:
                raise RuntimeError('Device rejected the pairing operation')
        raise RuntimeError('Pairing response timed out; check that this is the USB shell port')

    def command(self, text):
        self.port.reset_input_buffer()
        self.port.write((self.target + ' pair ' + text + '\r').encode('ascii'))

    def status(self):
        self.command('status')
        m = self.read_marker(rb'APX_PAIR_V1 (unpaired|paired ([0-9a-fA-F]{16}) ([0-9a-fA-F]{16}))')
        return None if m[1] == b'unpaired' else (m[2].decode().lower(), m[3].decode().lower())

    def write(self, record, replace=False):
        if len(record) != 32 or record[:4] != b'APB1' or zlib.crc32(record[:28]) != struct.unpack('<I', record[28:])[0]:
            raise ValueError('Invalid pairing record')
        self.command('write replace' if replace else 'write')
        self.read_marker(rb'APX_PAIR_READY 32')
        self.port.write(record)
        self.read_marker(rb'APX_PAIR_SAVED')
        expected = record[4:12].hex(), hashlib.sha256(record[4:28]).hexdigest()[:16]
        if self.status() != expected:
            raise RuntimeError('Pairing read-back fingerprint does not match')

    def clear(self):
        self.command('clear confirm')
        self.read_marker(rb'APX_PAIR_CLEARED')
        if self.status() is not None:
            raise RuntimeError('Pairing was not cleared')


def make_bond():
    body = b'APB1' + secrets.token_bytes(8) + secrets.token_bytes(16)
    return body + struct.pack('<I', zlib.crc32(body))


def _app_ports(vid_pid):
    return [p.device for p in list_ports.comports() if (p.vid, p.pid) == vid_pid]


def _answers_pair_shell(device, target, timeout=3.0):
    """True if the port responds to `<target> pair status`, i.e. it is the USB
    shell and not the keyboard's Studio port or some other CDC interface."""
    try:
        client = PairClient(device, target)
    except (OSError, serial.SerialException):
        return False
    try:
        client.command('status')
        client.read_marker(rb'APX_PAIR_V1 ', timeout=timeout)
        return True
    except Exception:
        return False
    finally:
        client.close()


def autodetect_port(vid_pid, target, label):
    """Find the one USB shell port for a device, so pairing needs no --*-port.
    The keyboard exposes more than one CDC port, so when several match the app
    USB ID we probe each and keep the one that answers the pairing shell."""
    candidates = _app_ports(vid_pid)
    if not candidates:
        raise RuntimeError(
            f'No {label} found on USB (looking for {vid_pid[0]:04x}:{vid_pid[1]:04x}). '
            f'Plug it in and close any open serial terminal, or pass --{label}-port.')
    if len(candidates) == 1:
        return candidates[0]
    shells = [d for d in candidates if _answers_pair_shell(d, target)]
    if len(shells) == 1:
        return shells[0]
    raise RuntimeError(
        f'Could not pick the {label} shell port automatically (found {", ".join(candidates)}). '
        f'Pass --{label}-port explicitly.')


def pair_devices(keyboard_port, dongle_port, replace=False):
    if keyboard_port == dongle_port:
        raise RuntimeError('Keyboard and dongle must use different ports')
    clients = []
    wrote = False
    try:
        for port, target in ((keyboard_port, 'apex'), (dongle_port, 'dongle')):
            clients.append(PairClient(port, target))
        states = [c.status() for c in clients]
        if any(s is not None for s in states) and not replace:
            raise RuntimeError('A device is already paired; use --replace to change its pairing')
        record = make_bond()
        for c, state in zip(clients, states):
            wrote = True
            c.write(record, replace=state is not None)
        print('Paired keyboard and dongle:', record[4:12].hex())
        print('Restart both so they load the new key: unplug and replug the dongle, '
              'then set the keyboard to dongle mode and unplug its USB.')
    except Exception:
        if wrote:
            print('Pairing was interrupted; a device may already have saved it. Reconnect both and repeat with --replace.')
        raise
    finally:
        for c in clients:
            c.close()


def bootloader_ports(serial_number=None):
    return [p for p in list_ports.comports()
            if (p.vid, p.pid) == (BOOT_VID, BOOT_PID)
            and (serial_number is None or p.serial_number == serial_number)]


def hold_bootloader(serial_number=None, timeout=30):
    """Latch recovery by asserting DTR during the startup window; write no flash."""
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        ports = bootloader_ports(serial_number)
        if len(ports) > 1:
            raise RuntimeError('More than one receiver found; specify --serial.')
        if ports:
            try:
                with serial.Serial(ports[0].device, 115200, timeout=0.2) as port:
                    port.dtr = True
                    time.sleep(0.2)
                return ports[0].device
            except (OSError, serial.SerialException) as exc:
                last_error = exc
        time.sleep(0.05)
    raise RuntimeError(f'Receiver bootloader not opened within {timeout}s.'
                       + (f' Last error: {last_error}' if last_error else ''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', help='USB serial number when several receivers are attached')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list', help='list connected receiver bootloaders')
    hold = commands.add_parser('hold-bootloader', help='run before reconnecting a hung receiver')
    hold.add_argument('--timeout', type=float, default=30, help='seconds to wait (default: 30)')
    pair = commands.add_parser('pair', help='pair keyboard and receiver over their USB shell ports')
    pair.add_argument('--keyboard-port', help='keyboard shell port (auto-detected if omitted)')
    pair.add_argument('--dongle-port', help='receiver shell port (auto-detected if omitted)')
    pair.add_argument('--replace', action='store_true', help='replace an existing pairing on either device')
    update = commands.add_parser('update', help='send a keyboard application through the receiver')
    update.add_argument('--dongle-port', required=True)
    update.add_argument('file', help='update-capable keyboard UF2')
    update.add_argument('--install', action='store_true', help='verify, install and restart after downloading')
    update.add_argument('--resume', action='store_true', help='continue a matching download if the keyboard has not restarted')
    update.add_argument('--allow-legacy-status', action='store_true',
                        help='development only: transfer to a binary-capable build that predates the bulk=1 status field')
    backup = commands.add_parser('backup', help='back up keyboard flash over its own USB shell')
    backup.add_argument('--keyboard-port', required=True)
    backup.add_argument('--output', required=True, help='new private backup directory')
    args = parser.parse_args()
    if args.command == 'list':
        for p in bootloader_ports(args.serial):
            print(p.device, p.serial_number)
    elif args.command == 'backup':
        from keyboard_backup import backup_keyboard
        try:
            backup_keyboard(args.keyboard_port, args.output)
        except (OSError, ValueError, RuntimeError, serial.SerialException) as exc:
            parser.exit(1, str(exc) + '\n')
    elif args.command == 'update':
        from wireless_update import update_keyboard
        try:
            update_keyboard(args.dongle_port, args.file, install=args.install, resume=args.resume,
                            require_bulk=not args.allow_legacy_status)
        except (OSError, ValueError, RuntimeError, serial.SerialException) as exc:
            parser.exit(1, str(exc) + '\n')
    elif args.command == 'pair':
        try:
            keyboard_port = args.keyboard_port or autodetect_port(KB_APP, 'apex', 'keyboard')
            dongle_port = args.dongle_port or autodetect_port(DG_APP, 'dongle', 'dongle')
            if not args.keyboard_port or not args.dongle_port:
                print(f'Using keyboard {keyboard_port} and dongle {dongle_port}.')
            pair_devices(keyboard_port, dongle_port, args.replace)
        except (RuntimeError, serial.SerialException) as exc:
            parser.exit(1, str(exc) + '\n')
    else:
        if not 0 < args.timeout <= 300:
            parser.error('--timeout must be between 0 and 300 seconds')
        print('Waiting for receiver bootloader. Unplug and reconnect the dongle.', flush=True)
        try:
            print('Bootloader held on', hold_bootloader(args.serial, args.timeout), flush=True)
        except RuntimeError as exc:
            parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
