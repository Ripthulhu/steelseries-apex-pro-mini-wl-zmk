"""Run the compiled bootloader's update/fallback decision with mocked flash I/O."""
import argparse
import hashlib
from pathlib import Path
import struct
import unittest
import zlib

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)


class BootTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for address, length in ((0, 0x80000), (0x20000000, 0x20000),
                                (0x40000000, 0x100000), (0x10000000, 0x20000),
                                (0x50000000, 0x10000)):
            self.cpu.mem_map(address, length)
        for address, data in SEGMENTS:
            self.cpu.mem_write(address, data)
        self.nor = bytearray(b'\xff' * 0x100000)
        self.erased = []
        self.returned = False
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, cpu, address, size, user):
        if address == 0x100:
            self.returned = True
            cpu.emu_stop()
            return
        name = HOOKS.get(address)
        if name is None:
            return
        a, pointer, n = [cpu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        result = 0
        if name == 'nor_read':
            self.assertLessEqual(a + n, len(self.nor))
            cpu.mem_write(pointer, bytes(self.nor[a:a + n]))
            result = 1
        elif name == 'update_nor_write':
            self.assertEqual((a, n), (0x82034, 4))
            data = bytes(cpu.mem_read(pointer, n))
            for i in range(n):
                self.nor[a + i] &= data[i]
        elif name == 'flash_nrf5x_erase':
            length = pointer
            self.assertTrue((0x1000 <= a and a + length <= 0x72000) or
                            (a, length) == (0x7f000, 4096))
            self.erased.append((a, length))
            cpu.mem_write(a, b'\xff' * length)
        elif name == 'flash_nrf5x_write':
            self.assertTrue(0x1000 <= a and a + n <= 0x72000)
            cpu.mem_write(a, bytes(cpu.mem_read(pointer, n)))
        cpu.reg_write(UC_ARM_REG_R0, result)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def run_boot(self):
        self.returned = False
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001f000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.cpu.emu_start(SYMBOLS['ab_promote_check'] | 1, 0,
                           timeout=15_000_000, count=3000000)
        self.assertTrue(self.returned)

    def image(self, value):
        image = bytearray([value] * 1024)
        struct.pack_into('<II', image, 0, 0x20020000, 0x1101)
        marker = b'APEX-KBD-OTA3\0'
        image[32:32 + len(marker)] = marker
        return bytes(image)

    def fallback(self, image):
        body = struct.pack('<8I', 0x41423447, 2, 0x1000, len(image), zlib.crc32(image), 0x8b000, 3, 1)
        self.nor[0x69000:0x69024] = body + struct.pack('<I', zlib.crc32(body))
        self.nor[0x8b000:0x8b000 + len(image)] = image

    def candidate(self, image, state=0xfffffffe):
        body = struct.pack('<4I', 0x55585041, 3, 0x41504b31, len(image)) + hashlib.sha256(image).digest()
        self.nor[0x82000:0x82038] = body + struct.pack('<II', zlib.crc32(body), state)
        self.nor[:len(image)] = image

    def test_installs_candidate_without_touching_fallback(self):
        old, new = self.image(0x11), self.image(0x22)
        self.fallback(old)
        self.candidate(new)
        self.run_boot()
        self.assertEqual(bytes(self.cpu.mem_read(0x1000, len(new))), new)
        self.assertEqual(self.nor[0x8b000:0x8b000 + len(old)], old)
        self.assertEqual(struct.unpack_from('<I', self.nor, 0x82034)[0], 0xfffffff8)

    def test_failed_trial_restores_existing_fallback(self):
        old, new = self.image(0x11), self.image(0x22)
        self.fallback(old)
        self.candidate(new, 0xfffffff8)
        self.cpu.mem_write(0x1000, new)
        self.nor[0x6a000:0x6a003] = b'\0' * 3
        self.run_boot()
        self.assertEqual(bytes(self.cpu.mem_read(0x1000, len(old))), old)

    def test_corrupt_candidate_uses_fallback(self):
        old = self.image(0x11)
        self.fallback(old)
        self.candidate(self.image(0x22))
        self.nor[600] ^= 1
        self.run_boot()
        self.assertEqual(bytes(self.cpu.mem_read(0x1000, len(old))), old)
        self.assertEqual(bytes(self.cpu.mem_read(0x4000051c, 4)), b'\0' * 4)

    def test_corrupt_candidate_keeps_running_fallback(self):
        old = self.image(0x11)
        self.fallback(old)
        self.cpu.mem_write(0x1000, old)
        self.candidate(self.image(0x22))
        self.nor[600] ^= 1
        self.run_boot()
        self.assertEqual(self.erased, [])
        self.assertEqual(bytes(self.cpu.mem_read(0x4000051c, 4)), b'\0' * 4)

    def test_legacy_no_request_does_not_erase(self):
        self.fallback(self.image(0x11))
        self.run_boot()
        self.assertEqual(self.erased, [])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    with args.elf.open('rb') as source:
        elf = ELFFile(source)
        SYMBOLS = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        SEGMENTS = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    HOOKS = {SYMBOLS[name] & ~1: name for name in (
        'nor_read', 'nor_close', 'update_nor_write', 'flash_nrf5x_erase',
        'flash_nrf5x_write', 'flash_nrf5x_flush')}
    # No-op the RGB install-progress bar (drives SPIM2 on real hardware).
    for opt in ('update_nor_wait', 'board_rgb_progress'):
        if opt in SYMBOLS:
            HOOKS[SYMBOLS[opt] & ~1] = opt
    unittest.main(argv=['boot-update-tests'], verbosity=2)
