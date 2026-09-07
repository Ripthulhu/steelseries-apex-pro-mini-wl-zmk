"""Run the receiver's compiled USB update parser with mocked radio and shell I/O."""
import argparse
from pathlib import Path
import struct
import unittest
import zlib
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0, 0x80000)
        self.cpu.mem_map(0x20000000, 0x20000)
        for address, data in SEGMENTS:
            self.cpu.mem_write(address, data)
        self.frames, self.replies = [], []
        self.radio_error = 0
        self.bypass = False
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)
        self.call('receiver_update_start', 0, 1, 0)

    def hook(self, cpu, address, size, user):
        if address == 0x100:
            self.returned = True
            cpu.emu_stop()
            return
        name = HOOKS.get(address)
        if not name:
            return
        a, b, c = (cpu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        result = 0
        if name in ('apex_radio_input_connected', 'apex_shell_generation'):
            result = 1
        elif name == 'z_impl_k_uptime_ticks':
            cpu.reg_write(UC_ARM_REG_R1, 0)
        elif name == 'shell_set_bypass':
            self.bypass = bool(b)
        elif name == 'apex_shell_send':
            self.frames.append(bytes(cpu.mem_read(b, c)))
            result = self.radio_error
        elif name == 'apex_shell_read':
            frame = self.frames[-1]
            received = struct.unpack_from('<I', frame, 1)[0] + len(frame) - 5
            cpu.mem_write(b, b'\x06' + struct.pack('<iI', 0, received))
            result = 9
        elif name == 'reply':
            self.replies.append((b, c))
        cpu.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def call(self, name, a, b, c):
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), (a, b, c)):
            self.cpu.reg_write(reg, value)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001f000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.returned = False
        self.cpu.emu_start(SYMBOLS[name] | 1, 0, timeout=1_000_000, count=200000)
        self.assertTrue(self.returned)

    def feed(self, data):
        for offset in range(0, len(data), 16):
            part = data[offset:offset + 16]
            self.cpu.mem_write(0x2001d000, bytes(part))
            self.call('receive', 0, 0x2001d000, len(part))
            if not self.bypass:
                break

    def frame(self, data=b'x' * 256, offset=0):
        packet = struct.pack('<4sIHH', b'AUP1', offset, len(data), 0) + data
        return packet + struct.pack('<I', zlib.crc32(packet))

    def test_complete_block_and_finish(self):
        self.feed(self.frame())
        self.assertEqual(len(self.frames), 8)
        self.assertEqual(b''.join(f[5:] for f in self.frames), b'x' * 256)
        self.assertEqual(self.replies, [(0, 256)])
        self.feed(self.frame(b'', 256))
        self.assertFalse(self.bypass)

    def test_bad_crc_never_reaches_radio(self):
        packet = bytearray(self.frame())
        packet[-1] ^= 1
        self.feed(packet)
        self.assertEqual(self.frames, [])
        self.assertNotEqual(self.replies[-1][0], 0)
        self.assertFalse(self.bypass)

    def test_invalid_length_and_sector_boundary(self):
        for offset, length in ((0, 257), (4090, 32)):
            self.setUp()
            self.feed(struct.pack('<4sIHH', b'AUP1', offset, length, 0))
            self.assertEqual(self.frames, [])
            self.assertFalse(self.bypass)

    def test_incomplete_block_never_reaches_radio(self):
        self.feed(self.frame()[:120])
        self.assertEqual(self.frames, [])
        self.assertEqual(self.replies, [])

    def test_radio_error_stops_transfer(self):
        self.radio_error = -125
        self.feed(self.frame())
        self.assertEqual(len(self.frames), 1)
        self.assertEqual(self.replies[-1][0], 0xffffff83)
        self.assertFalse(self.bypass)


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
        'apex_radio_input_connected', 'apex_radio_request_session', 'apex_shell_generation',
        'apex_shell_send', 'apex_shell_read', 'z_impl_k_uptime_ticks', 'shell_set_bypass',
        'shell_fprintf_normal', 'reply')}
    unittest.main(argv=['binary-update-tests'], verbosity=2)
