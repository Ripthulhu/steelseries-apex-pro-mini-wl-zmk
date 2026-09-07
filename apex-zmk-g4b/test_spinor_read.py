#!/usr/bin/env python3
"""Test the compiled NOR read loop with SPI transfers replaced by a memory model.

Pass the keyboard ELF. Requires pyelftools and unicorn in the build environment.
No hardware is accessed. This checks chunking and errors, not SPI timing.
"""
import argparse
from pathlib import Path
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
)

DEST = 0x20021000


class ReadTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0, 0x80000)
        self.cpu.mem_map(0x20000000, 0x40000)
        for addr, data in SEGMENTS:
            self.cpu.mem_write(addr, data)
        self.calls = []
        self.fail_at = None
        self.returned = False
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    @staticmethod
    def pattern(address, length):
        return bytes((v ^ (v >> 8) ^ (v >> 16)) & 255
                     for v in range(address, address + length))

    def hook(self, cpu, address, size, user):
        if address == 0x100:
            self.returned = True
            cpu.emu_stop()
        elif address == (SYMBOLS['nor_xfer'] & ~1):
            cmdlen = cpu.reg_read(UC_ARM_REG_R0)
            out = cpu.reg_read(UC_ARM_REG_R1)
            length = cpu.reg_read(UC_ARM_REG_R2)
            command = bytes(cpu.mem_read(SYMBOLS['nor_txbuf'], 4))
            self.assertEqual(cmdlen, 4)
            self.assertEqual(command[0], 3)
            self.assertLessEqual(length, 1024)
            self.assertGreater(length, 0)
            addr = int.from_bytes(command[1:], 'big')
            offset = len(self.calls) * 1024
            self.assertEqual(addr, self.read_address + offset)
            self.assertEqual(out, self.read_output + offset)
            self.assertEqual(length, min(1024, self.read_length - offset))
            self.calls.append((addr, out, length))
            ok = len(self.calls) != self.fail_at
            if ok:
                cpu.mem_write(out, self.pattern(addr, length))
            cpu.reg_write(UC_ARM_REG_R0, int(ok))
            cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def call(self, function, *args):
        self.returned = False
        self.read_address, self.read_output, self.read_length = args
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            self.cpu.reg_write(reg, value)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001F000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.cpu.emu_start(SYMBOLS[function] | 1, 0, timeout=2_000_000, count=100000)
        self.assertTrue(self.returned)
        return self.cpu.reg_read(UC_ARM_REG_R0)

    def test_boundaries(self):
        for length in (0, 1, 1023, 1024, 1025, 4096, 4097, 8193):
            with self.subTest(length=length):
                self.calls.clear()
                self.cpu.mem_write(DEST - 8, b'\xA5' * (length + 16))
                self.assertEqual(self.call('nor_read', 0x123F1, DEST, length), 1)
                self.assertEqual(bytes(self.cpu.mem_read(DEST - 8, length + 16)),
                                 b'\xA5' * 8 + self.pattern(0x123F1, length) + b'\xA5' * 8)
                self.assertEqual(len(self.calls), (length + 1023) // 1024)

    def test_failure_stops_read(self):
        for failure in range(1, 5):
            with self.subTest(failure=failure):
                self.calls.clear()
                self.fail_at = failure
                self.cpu.mem_write(DEST, b'\xA5' * 4096)
                self.assertEqual(self.call('nor_read', 0xFF000, DEST, 4096), 0)
                self.assertEqual(len(self.calls), failure)
                done = (failure - 1) * 1024
                self.assertEqual(bytes(self.cpu.mem_read(DEST, 4096)),
                                 self.pattern(0xFF000, done) + b'\xA5' * (4096 - done))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    with args.elf.open('rb') as file:
        elf = ELFFile(file)
        SEGMENTS = [(s['p_vaddr'], s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
        SYMBOLS = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    # GCC may outline the hardware transaction after proving the size checks.
    if 'nor_xfer' not in SYMBOLS:
        SYMBOLS['nor_xfer'] = SYMBOLS['nor_xfer.part.0']
    unittest.main(argv=['test_spinor_read'])
