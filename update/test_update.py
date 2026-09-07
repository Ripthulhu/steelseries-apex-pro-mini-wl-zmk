"""Exercise the C update engine with simulated flash and interrupted writes.

Run with a native C compiler and --tinycrypt pointing to its lib directory.
No device is accessed. Temporary build output is removed when the tests finish.
"""
import argparse
import ctypes as C
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

CAPACITY = 0x71000
JOURNAL = 0x82000
MARKER = b'APEX-KBD-OTA3'
RW = C.CFUNCTYPE(C.c_int, C.c_uint32, C.c_void_p, C.c_uint32)
ERASE = C.CFUNCTYPE(C.c_int, C.c_uint32, C.c_uint32)
FINISH = C.CFUNCTYPE(C.c_int)


class IO(C.Structure):
    _fields_ = [('nor_read', RW), ('nor_write', RW), ('nor_erase', ERASE),
                ('app_read', RW), ('app_write', RW), ('app_erase', ERASE),
                ('app_finish', FINISH)]


class Download(C.Structure):
    _fields_ = [('manifest', C.c_uint8 * 56), ('received', C.c_uint32), ('active', C.c_bool)]


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.nor = bytearray(b'\xff' * 0x100000)
        self.app = bytearray(b'\xa5' * 0x80000)
        self.cut = None
        self.operations = 0
        self.bad_access = False
        self.io = IO(RW(lambda a, p, n: self.read(self.nor, a, p, n)),
                     RW(lambda a, p, n: self.write(self.nor, a, p, n)),
                     ERASE(lambda a, n: self.erase(self.nor, a, n)),
                     RW(lambda a, p, n: self.read(self.app, a, p, n)),
                     RW(lambda a, p, n: self.write(self.app, a, p, n)),
                     ERASE(lambda a, n: self.erase(self.app, a, n)), FINISH(lambda: self.mutation()))
        self.download = Download()

    def read(self, memory, address, pointer, size):
        if address + size > len(memory):
            self.bad_access = True
            return -1
        C.memmove(pointer, bytes(memory[address:address + size]), size)
        return 0

    def mutation(self):
        self.operations += 1
        return -1 if self.cut is not None and self.operations >= self.cut else 0

    def allowed(self, memory, address, size):
        ranges = ((0x1000, 0x72000),) if memory is self.app else (
            (0, 0x60000), (0x77000, 0x80000), (0x82000, 0x8b000))
        return any(start <= address and address + size <= end for start, end in ranges)

    def write(self, memory, address, pointer, size):
        if not self.allowed(memory, address, size):
            self.bad_access = True
            return -1
        data = C.string_at(pointer, size)
        failed = self.mutation()
        # A cut may leave part of a page programmed.
        count = size // 2 if failed else size
        for i in range(count):
            memory[address + i] &= data[i]
        return failed

    def erase(self, memory, address, size):
        if address % 4096 or size != 4096 or not self.allowed(memory, address, size):
            self.bad_access = True
            return -1
        failed = self.mutation()
        count = size // 2 if failed else size
        memory[address:address + count] = b'\xff' * count
        return failed

    def image(self, length=8192):
        image = bytearray((i % 251 for i in range(length)))
        image[:8] = struct.pack('<II', 0x20020000, 0x1101)
        image[253:253 + len(MARKER)] = MARKER
        return bytes(image)

    def begin(self, image):
        return LIB.apx_update_begin(C.byref(self.io), C.byref(self.download), len(image),
                                    hashlib.sha256(image).digest())

    def write_chunk(self, offset, data):
        return LIB.apx_update_write(C.byref(self.io), C.byref(self.download), offset, data, len(data))

    def stage(self, image):
        self.assertEqual(self.begin(image), 0)
        for offset in range(0, len(image), 256):
            self.assertEqual(self.write_chunk(offset, image[offset:offset + 256]), 0)
        self.assertEqual(LIB.apx_update_request(C.byref(self.io), C.byref(self.download)), 0)

    def state(self):
        return struct.unpack_from('<I', self.nor, JOURNAL + 52)[0]

    def test_full_capacity_and_protected_regions(self):
        image = self.image(CAPACITY)
        self.stage(image)
        self.assertEqual(LIB.apx_update_install(C.byref(self.io)), 1)
        self.assertEqual(self.app[0x1000:0x72000], image)
        self.assertEqual(self.state(), 0xfffffff8)
        self.assertEqual(LIB.apx_update_health(C.byref(self.io)), 0)
        self.assertEqual(self.state(), 0xfffffff0)
        self.assertEqual(self.nor[0x60000:0x77000], b'\xff' * 0x17000)
        self.assertEqual(self.nor[0x8b000:], b'\xff' * 0x75000)
        self.assertEqual(self.app[:0x1000], b'\xa5' * 0x1000)
        self.assertEqual(self.app[0x72000:], b'\xa5' * 0xe000)
        self.assertFalse(self.bad_access)

    def test_reset_at_each_install_mutation(self):
        image = self.image()
        self.stage(image)
        original_nor, original_app = self.nor[:], self.app[:]
        self.operations = 0
        self.assertEqual(LIB.apx_update_install(C.byref(self.io)), 1)
        count = self.operations
        for cut in range(1, count + 1):
            with self.subTest(cut=cut):
                self.nor[:], self.app[:] = original_nor, original_app
                self.operations = 0
                self.cut = cut
                self.assertLess(LIB.apx_update_install(C.byref(self.io)), 0)
                self.cut = None
                self.assertIn(LIB.apx_update_install(C.byref(self.io)), (0, 1))
                self.assertEqual(self.app[0x1000:0x1000 + len(image)], image)
                self.assertFalse(self.bad_access)

    def test_duplicates_order_and_truncation(self):
        image = self.image()
        self.assertEqual(self.begin(image), 0)
        self.assertLess(self.write_chunk(256, image[256:512]), 0)
        self.assertEqual(self.write_chunk(0, image[:256]), 0)
        count = self.operations
        self.assertEqual(self.write_chunk(0, image[:256]), 0)
        self.assertEqual(self.operations, count)
        self.assertLess(self.write_chunk(0, b'\0' * 256), 0)
        self.assertLess(LIB.apx_update_request(C.byref(self.io), C.byref(self.download)), 0)
        self.assertEqual(LIB.apx_update_install(C.byref(self.io)), 0)

    def test_corruption_and_wrong_board(self):
        self.stage(self.image())
        old_app = self.app[:]
        self.nor[512] ^= 1
        self.assertLess(LIB.apx_update_install(C.byref(self.io)), 0)
        self.assertEqual(self.app, old_app)
        self.nor[512] ^= 1
        self.nor[JOURNAL + 8] ^= 1
        self.assertLess(LIB.apx_update_install(C.byref(self.io)), 0)
        self.assertEqual(self.app, old_app)

    def test_vectors_and_board_marker(self):
        for position in (0, 4, 253):
            self.setUp()
            image = bytearray(self.image())
            image[position:position + 4] = b'\0' * 4
            self.assertEqual(self.begin(image), 0)
            for offset in range(0, len(image), 256):
                self.assertEqual(self.write_chunk(offset, bytes(image[offset:offset + 256])), 0)
            self.assertLess(LIB.apx_update_request(C.byref(self.io), C.byref(self.download)), 0)

    def test_trial_cannot_be_overwritten(self):
        image = self.image()
        self.stage(image)
        self.assertLess(self.begin(image), 0)
        self.assertEqual(LIB.apx_update_install(C.byref(self.io)), 1)
        self.assertLess(self.begin(image), 0)
        self.assertEqual(LIB.apx_update_health(C.byref(self.io)), 0)
        self.assertEqual(self.begin(image), 0)

    def test_fallback_health_rejects_candidate(self):
        self.stage(self.image())
        self.assertEqual(LIB.apx_update_install(C.byref(self.io)), 1)
        self.app[0x1200] ^= 1
        self.assertEqual(LIB.apx_update_health(C.byref(self.io)), 0)
        self.assertEqual(self.state(), 0xffffffe0)

    def test_interrupted_begin_cannot_request_installation(self):
        for cut in (1, 2):
            self.setUp()
            self.cut = cut
            self.assertLess(self.begin(self.image()), 0)
            self.cut = None
            self.assertEqual(LIB.apx_update_install(C.byref(self.io)), 0)
            self.assertEqual(self.app, b'\xa5' * 0x80000)

    def test_invalid_sizes_do_not_erase(self):
        for size in (0, 4, 7, 9, CAPACITY + 4):
            self.assertLess(LIB.apx_update_begin(C.byref(self.io), C.byref(self.download),
                                               size, b'\0' * 32), 0)
        self.assertEqual(self.operations, 0)

    def test_filesystem_must_be_blank_or_recognized_empty_volume(self):
        self.assertEqual(LIB.apx_update_check_filesystem(C.byref(self.io)), 0)
        header = bytes.fromhex(
            '01000000f00ffff76c6974746c6566732fe00010010002000010000015000000'
            'ff000000ffffff7ffe0300007feffc1010000000e5394cc00ff0000caa34ff20')
        self.nor[0x6b000:0x6b040] = header
        self.assertEqual(LIB.apx_update_check_filesystem(C.byref(self.io)), 0)
        self.nor[0x7fff0] = 0
        self.assertLess(LIB.apx_update_check_filesystem(C.byref(self.io)), 0)
        self.nor[0x7fff0] = 255
        self.nor[0x6b01c] ^= 1
        self.assertLess(LIB.apx_update_check_filesystem(C.byref(self.io)), 0)
        self.assertEqual(self.operations, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tinycrypt', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as temp:
        library = Path(temp) / 'update.so'
        subprocess.run([args.cc, '-shared', '-fPIC', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(args.tinycrypt / 'include'), str(root / 'apex_update.c'),
                        str(args.tinycrypt / 'source/sha256.c'),
                        str(args.tinycrypt / 'source/utils.c'), '-o', str(library)], check=True)
        LIB = C.CDLL(str(library))
        LIB.apx_update_begin.argtypes = [C.POINTER(IO), C.POINTER(Download), C.c_uint32, C.c_char_p]
        LIB.apx_update_write.argtypes = [C.POINTER(IO), C.POINTER(Download), C.c_uint32,
                                        C.c_char_p, C.c_uint32]
        result = unittest.main(argv=['update-tests'], verbosity=2, exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
