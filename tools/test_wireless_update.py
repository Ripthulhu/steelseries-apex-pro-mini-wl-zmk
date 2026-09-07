"""Validate UF2 input before opening a serial port."""
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
from unittest.mock import patch

from wireless_update import read_image, UpdateClient, CAPACITY, MARKER


def uf2(image):
    blocks = []
    for i in range(len(image) // 256):
        block = bytearray(512)
        struct.pack_into('<8I', block, 0, 0x0a324655, 0x9e5d5157, 0x2000,
                         0x1000 + 256 * i, 256, i, len(image) // 256, 0x621e937a)
        block[32:288] = image[i * 256:(i + 1) * 256]
        struct.pack_into('<I', block, 508, 0x0ab16f30)
        blocks.append(block)
    return b''.join(blocks)


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.image = bytearray(b'\xff' * 512)
        struct.pack_into('<II', self.image, 0, 0x20020000, 0x1101)
        self.image[32:32 + len(MARKER)] = MARKER
        self.file = uf2(self.image)

    def read(self, data):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'keyboard.uf2'
            path.write_bytes(data)
            return read_image(path)

    def test_valid_and_reordered(self):
        self.assertEqual(self.read(self.file), self.image)
        self.assertEqual(self.read(self.file[512:] + self.file[:512]), self.image)

    def test_rejects_truncated_duplicate_and_empty(self):
        for data in (b'', self.file[:-1], self.file[:512], self.file[:512] * 2):
            with self.assertRaises(ValueError):
                self.read(data)

    def test_rejects_bootloader_and_out_of_bounds(self):
        for address in (0, 0x74000, 0x10001000, 0x1000 + CAPACITY, 0x1001):
            data = bytearray(self.file)
            struct.pack_into('<I', data, 12, address)
            with self.assertRaises(ValueError):
                self.read(data)

    def test_rejects_wrong_flags_family_and_size(self):
        for position, value in ((8, 0), (8, 0x2001), (16, 128), (28, 0)):
            data = bytearray(self.file)
            struct.pack_into('<I', data, position, value)
            with self.assertRaises(ValueError):
                self.read(data)

    def test_rejects_wrong_board_marker_and_vectors(self):
        for position, value in ((0, 0), (0, 0x20020008), (4, 0x1100), (4, 0x74001), (32, 0)):
            image = self.image[:]
            struct.pack_into('<I', image, position, value)
            with self.assertRaises(ValueError):
                self.read(uf2(image))


class FakePort:
    def __init__(self, reply):
        self.reply = reply
        self.pending = b''
        self.writes = []

    @property
    def in_waiting(self):
        return min(len(self.pending), 7)

    def reset_input_buffer(self):
        self.pending = b''

    def write(self, data):
        self.writes.append(data)
        if data.endswith(b'\r'):
            self.pending = self.reply

    def flush(self):
        pass

    def read(self, count):
        data, self.pending = self.pending[:count], self.pending[count:]
        return data


class ClientTests(unittest.TestCase):
    def client(self, reply):
        client = object.__new__(UpdateClient)
        client.port = FakePort(reply)
        return client

    def test_paces_long_commands_and_parses_fragmented_reply(self):
        client = self.client(b'\x1b[32mUPDATE result=0 received=32\r\n[keyboard result: 0]\r\n')
        command = 'write 0 ' + 'ab' * 32
        with patch('wireless_update.time.sleep'):
            _, received = client.command(command)
        self.assertEqual(received, 32)
        self.assertEqual(b''.join(client.port.writes), ('keyboard update ' + command + '\r').encode())
        self.assertTrue(all(len(w) <= 32 for w in client.port.writes))

    def test_rejects_missing_or_failed_result(self):
        for reply in (b'[keyboard result: 0]', b'UPDATE result=-2 received=0\n[keyboard result: -2]'):
            with self.assertRaises(RuntimeError):
                self.client(reply).command('commit')

    def test_overflow_stops_without_repeating_command(self):
        client = self.client(b'RX ring buffer full')
        with self.assertRaisesRegex(RuntimeError, 'overflowed'):
            client.command('commit')
        self.assertEqual(client.port.writes, [b'keyboard update commit\r'])

    def test_lost_reply_is_not_retried(self):
        client = self.client(b'')
        with patch('wireless_update.time.monotonic', side_effect=[0, 0, 21]):
            with self.assertRaisesRegex(RuntimeError, 'not retried'):
                client.command('commit')
        self.assertEqual(client.port.writes, [b'keyboard update commit\r'])


class BackupTests(unittest.TestCase):
    def test_read_only_dump_parsing(self):
        from keyboard_backup import read_chunk
        reply = (b'00001000: ' + b'12 ' * 16 + b'| ................\n' +
                 b'00001010: ' + b'ab ' * 16 + b'| ................\n')
        port = FakePort(reply)
        self.assertEqual(read_chunk(port, 'ext_flash@0 ', 0x1000, 32),
                         b'\x12' * 16 + b'\xab' * 16)
        self.assertEqual(port.writes, [b'flash read ext_flash@0 1000 20\r'])

    def test_incomplete_dump_is_rejected(self):
        from keyboard_backup import read_chunk
        with patch('keyboard_backup.time.monotonic', side_effect=[0, 0, 16]):
            with self.assertRaisesRegex(RuntimeError, 'Incomplete'):
                read_chunk(FakePort(b''), '', 0, 32)


class BinaryPort(FakePort):
    def write(self, data):
        self.writes.append(data)
        self.pending = self.reply


class BinaryTests(unittest.TestCase):
    def client(self, received, result=0, corrupt=False):
        record = struct.pack('<4sIi', b'AUR1', received, result)
        record += struct.pack('<I', zlib.crc32(record) ^ int(corrupt))
        client = object.__new__(UpdateClient)
        client.bulk = True
        client.port = BinaryPort(b'dongle:~$ ' + record)
        return client

    def test_binary_block_and_crc(self):
        client = self.client(256)
        self.assertEqual(client.bulk_write(0, b'x' * 256), 256)
        frame = client.port.writes[0]
        self.assertEqual(struct.unpack_from('<4sIHH', frame), (b'AUP1', 0, 256, 0))
        self.assertEqual(frame[12:-4], b'x' * 256)
        self.assertEqual(zlib.crc32(frame[:-4]), struct.unpack_from('<I', frame, len(frame)-4)[0])

    def test_bad_acknowledgements_stop(self):
        for client in (self.client(255), self.client(256, -2), self.client(256, corrupt=True)):
            with self.assertRaises(RuntimeError):
                client.bulk_write(0, b'x' * 256)
            self.assertEqual(len(client.port.writes), 1)

    def test_no_cross_sector_block(self):
        client = self.client(0)
        with self.assertRaises(ValueError):
            client.bulk_write(4090, b'x' * 32)
        self.assertEqual(client.port.writes, [])

    def test_end_returns_to_shell(self):
        client = self.client(256)
        client.bulk_write(256, b'')
        self.assertFalse(client.bulk)


if __name__ == '__main__':
    unittest.main(verbosity=2)
