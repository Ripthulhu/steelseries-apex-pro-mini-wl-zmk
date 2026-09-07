"""Keyboard application updates over the receiver's authenticated shell link."""
import hashlib
from pathlib import Path
import re
import struct
import time
import zlib

CAPACITY = 0x71000
APP_BASE = 0x1000
MARKER = b'APEX-KBD-OTA3'


def read_image(path):
    path = Path(path)
    if path.stat().st_size > CAPACITY * 2:
        raise ValueError('UF2 exceeds the keyboard application capacity')
    raw = path.read_bytes()
    if not raw or len(raw) % 512:
        raise ValueError('Expected a complete keyboard UF2 file')
    blocks = {}
    addresses = set()
    count = len(raw) // 512
    image = bytearray(b'\xff' * CAPACITY)
    end = APP_BASE
    for offset in range(0, len(raw), 512):
        block = raw[offset:offset + 512]
        magic0, magic1, flags, address, size, index, total, family = struct.unpack_from('<8I', block)
        if (magic0, magic1, struct.unpack_from('<I', block, 508)[0]) != (
                0x0a324655, 0x9e5d5157, 0x0ab16f30):
            raise ValueError('Invalid UF2 block')
        if flags != 0x2000 or family != 0x621e937a or size != 256 or total != count or index >= total:
            raise ValueError('Unsupported UF2 flags, family, payload or block count')
        if address < APP_BASE or address + size > APP_BASE + CAPACITY or address % 256:
            raise ValueError('UF2 contains data outside the keyboard application slot')
        if index in blocks or address in addresses:
            raise ValueError('UF2 contains duplicate blocks')
        blocks[index] = address
        addresses.add(address)
        image[address - APP_BASE:address - APP_BASE + size] = block[32:32 + size]
        end = max(end, address + size)
    if APP_BASE not in addresses:
        raise ValueError('UF2 does not contain the application vectors')
    image = bytes(image[:end - APP_BASE])
    stack, reset = struct.unpack_from('<II', image)
    if not (0x20000000 < stack <= 0x20020000 and stack % 8 == 0 and reset & 1 and
            APP_BASE <= (reset & ~1) < end):
        raise ValueError('Invalid keyboard application vectors')
    if MARKER not in image:
        raise ValueError('This is not an update-capable keyboard build')
    return image


class UpdateClient:
    def __init__(self, port):
        import serial
        self.port = serial.Serial(port, 115200, timeout=.1, write_timeout=2)
        self.bulk = False
        try:
            self.port.dtr = True
            time.sleep(2.1)
            self.port.write(b'\x03\r')
            time.sleep(.3)
            self.port.reset_input_buffer()
        except Exception:
            self.port.close()
            raise

    def close(self):
        if self.bulk:
            # Let a partial USB block expire, then cancel bypass at a frame boundary.
            time.sleep(2.1)
            try:
                self.port.write(b'\x03\r')
            except OSError:
                pass
        self.port.close()

    def bulk_start(self):
        self.port.reset_input_buffer()
        self.bulk = True
        self.port.write(b'dongle upload\r')
        response = b''
        end = time.monotonic() + 5
        while time.monotonic() < end and len(response) < 4096:
            response += self.port.read(min(self.port.in_waiting, 1024) or 1)
            if b'APX-UPLOAD-1 READY' in response:
                return
        raise RuntimeError('Receiver did not enter binary update mode; it may need newer firmware')

    def bulk_write(self, offset, data):
        if len(data) > 256 or len(data) > 4096 - offset % 4096:
            raise ValueError('Invalid update block size')
        frame = struct.pack('<4sIHH', b'AUP1', offset, len(data), 0) + data
        frame += struct.pack('<I', zlib.crc32(frame))
        self.port.write(frame)
        response = b''
        end = time.monotonic() + 5
        while time.monotonic() < end and len(response) < 4096:
            response += self.port.read(min(self.port.in_waiting, 1024) or 1)
            index = response.find(b'AUR1')
            if index < 0 or len(response) < index + 16:
                continue
            record = response[index:index + 16]
            received, result, crc = struct.unpack_from('<IiI', record, 4)
            if zlib.crc32(record[:12]) != crc:
                raise RuntimeError('Corrupt USB update acknowledgement; transfer stopped')
            if result or received != offset + len(data):
                raise RuntimeError(f'Update block rejected: result={result}, received={received}')
            if not data:
                self.bulk = False
            return received
        raise RuntimeError('Binary update reply lost; inspect status before resuming')

    def reconnect(self):
        self.port.reset_input_buffer()
        self.port.write(b'dongle reconnect\r')
        response = b''
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            response += self.port.read(min(self.port.in_waiting, 1024) or 1)
            if b'Radio session restarting.' in response:
                time.sleep(1)
                return
            if len(response) > 4096:
                break
        raise RuntimeError('Receiver needs firmware with the reconnect command')

    def command(self, command):
        self.port.reset_input_buffer()
        line = ('keyboard update ' + command + '\r').encode('ascii')
        # Older receiver builds have a 64-byte USB shell receive ring.
        for offset in range(0, len(line), 32):
            self.port.write(line[offset:offset + 32])
            self.port.flush()
            if offset + 32 < len(line):
                time.sleep(.005)
        deadline = time.monotonic() + 20
        response = b''
        while time.monotonic() < deadline:
            response += self.port.read(min(self.port.in_waiting, 1024) or 1)
            if len(response) > 16384:
                raise RuntimeError('Unexpected update response size')
            clean = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', response)
            if b'RX ring buffer full' in clean:
                raise RuntimeError('Receiver USB shell overflowed; command outcome is unknown')
            result = re.search(rb'\[keyboard result: (-?\d+)\]', clean)
            if result:
                status = re.search(rb'UPDATE result=(-?\d+) received=(\d+)', clean)
                if int(result[1]) != 0 or not status or int(status[1]) != 0:
                    raise RuntimeError('Keyboard rejected update command: ' + clean.decode(errors='replace'))
                return clean, int(status[2])
            if b'reply incomplete' in clean or b'Keyboard is not connected' in clean:
                break
        verb = command.split()[0]
        raise RuntimeError(f'Update {verb} reply was lost. The command was not retried; check keyboard update status.')


def update_keyboard(port, path, *, install=False, resume=False, require_bulk=True):
    image = read_image(path)
    digest = hashlib.sha256(image).hexdigest()
    print(f'Keyboard application: {len(image)} bytes, SHA-256 {digest}', flush=True)
    client = UpdateClient(port)
    try:
        # Start with fresh fragment counters so a previous diagnostic session
        # cannot exhaust its 16-bit sequence numbers halfway through the image.
        client.reconnect()
        status, received = client.command('status')
        if not re.search(rb'layout=3 prepared=1 bootloader=1 ready=1', status):
            raise RuntimeError('Keyboard needs wired update preparation and a verified fallback first')
        if require_bulk and b'bulk=1' not in status:
            raise RuntimeError('Keyboard needs firmware with binary update support; install it over USB first')
        if not require_bulk and b'bulk=1' not in status:
            print('Development override: proceeding without bulk=1 status. The running '
                  'build must still support binary transfers.', flush=True)
        start = 0
        if resume:
            if (f'UPDATE sha256={digest}'.encode() not in status or
                    b'state=ffffffff' not in status or not 0 < received <= len(image) or received % 32):
                raise RuntimeError('No matching download to resume in this boot; inspect status before starting again')
            start = received
            print(f'Resuming at {start}/{len(image)} bytes', flush=True)
        else:
            client.command(f'begin {len(image)} {digest}')
        client.bulk_start()
        offset = start
        started = time.monotonic()
        while offset < len(image):
            length = min(256, len(image) - offset, 4096 - offset % 4096)
            received = client.bulk_write(offset, image[offset:offset + length])
            offset = received
            if received % 4096 == 0 or received == len(image):
                elapsed = max(time.monotonic() - started, .001)
                rate = (received - start) / elapsed
                print(f'Uploaded {received}/{len(image)} bytes ({rate:.0f} B/s)', flush=True)
        client.bulk_write(len(image), b'')
        if not install:
            print('Downloaded only. Running firmware is unchanged; no installation was requested.')
            return
        client.command('commit')
        print('Image verified. Restarting the keyboard to install it.', flush=True)
        # A reboot deliberately interrupts its own reply. Do not retry it.
        client.port.write(b'keyboard update reboot\r')
        time.sleep(5)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                status, _ = client.command('status')
            except RuntimeError:
                time.sleep(2)
                continue
            if f'UPDATE sha256={digest}'.encode() not in status:
                raise RuntimeError('Keyboard reports a different update image')
            if b'state=ffffffe0' in status:
                raise RuntimeError('The new application failed; keyboard recovered its fallback')
            if b'state=fffffff0' in status and b'ready=1' in status:
                print('Keyboard update passed its health checks and fallback is ready.')
                return
            time.sleep(2)
        raise RuntimeError('Installation outcome not confirmed. Check keyboard update status; do not resend automatically.')
    finally:
        client.close()
