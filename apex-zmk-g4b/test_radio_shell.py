"""Exercise the compiled radio shell command assembler with mocked link and shell APIs."""
import argparse
from pathlib import Path
import struct
import unittest
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                              UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

COUNT = 0x2001c000


class ShellTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0, 0x80000)
        self.cpu.mem_map(0x20000000, 0x20000)
        for addr, data in SEGMENTS:
            self.cpu.mem_write(addr, data)
        self.generation = 1
        self.pending = []
        self.commands = []
        self.replies = []
        self.result = 0
        self.writes = []
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, cpu, address, size, user):
        if address == 0x100:
            self.returned = True
            cpu.emu_stop()
            return
        name = HOOKS.get(address)
        if not name:
            return
        value = 0
        if name == 'apex_shell_generation':
            value = self.generation
        elif name == 'apex_shell_read':
            if self.pending:
                data = self.pending.pop(0)
                cpu.mem_write(cpu.reg_read(UC_ARM_REG_R1), data)
                value = len(data)
        elif name == 'apex_shell_send':
            self.replies.append(bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R1), cpu.reg_read(UC_ARM_REG_R2))))
        elif name == 'shell_execute_cmd':
            command = bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R1), 128)).split(b'\0', 1)[0]
            self.commands.append(command)
            value = self.result
        elif name == 'z_impl_k_uptime_ticks':
            cpu.reg_write(UC_ARM_REG_R1, 0)
        elif name == 'g4b_update_binary':
            offset = cpu.reg_read(UC_ARM_REG_R0)
            length = cpu.reg_read(UC_ARM_REG_R2)
            self.writes.append((offset, bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R1), length))))
            cpu.mem_write(cpu.reg_read(UC_ARM_REG_R3), struct.pack('<I', offset + length))
            value = self.result
        cpu.reg_write(UC_ARM_REG_R0, value & 0xffffffff)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def feed(self, data, final=True, kind=None):
        self.pending.append(bytes([kind if kind is not None else 2 if final else 1]) + data)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), (0, 0, 0, COUNT)):
            self.cpu.reg_write(reg, value)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001e000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.returned = False
        self.cpu.emu_start(SYMBOLS['transport_read'] | 1, 0, timeout=1_000_000, count=100000)
        self.assertTrue(self.returned)
        self.assertEqual(bytes(self.cpu.mem_read(COUNT, 4)), b'\0'*4)

    def test_fragmented_command_and_return_code(self):
        self.feed(b'apex ', final=False)
        self.assertEqual(self.commands, [])
        self.result = -22
        self.feed(b'battery')
        self.assertEqual(self.commands, [b'apex battery'])
        self.assertEqual(self.replies, [b'\x04' + struct.pack('<i', -22)])

    def test_session_reset_discards_partial_command(self):
        self.feed(b'apex charge ', final=False)
        self.generation += 1
        self.feed(b'apex battery')
        self.assertEqual(self.commands, [b'apex battery'])

    def test_rejects_oversized_command_then_recovers(self):
        for _ in range(3):
            self.feed(b'x'*46, final=False)
        self.feed(b'x')
        self.assertEqual(self.commands, [])
        self.assertLess(struct.unpack('<i', self.replies[-1][1:])[0], 0)
        self.feed(b'apex battery')
        self.assertEqual(self.commands, [b'apex battery'])

    def test_rejects_controls_and_other_roots(self):
        for command in (b'flash erase', b'apex\nreboot', b'apex\0charge', b'apex\x1b[A', b''):
            self.feed(command)
        self.assertEqual(self.commands, [])
        self.assertTrue(all(struct.unpack('<i', reply[1:])[0] < 0 for reply in self.replies))

    def test_binary_write_returns_position_and_error(self):
        if 'g4b_update_binary' not in SYMBOLS:
            self.skipTest('Update support is disabled')
        self.feed(struct.pack('<I', 32) + b'x' * 32, kind=5)
        self.assertEqual(self.writes, [(32, b'x' * 32)])
        self.assertEqual(self.replies[-1], b'\x06' + struct.pack('<iI', 0, 64))
        self.result = -11
        self.feed(struct.pack('<I', 64) + b'y' * 32, kind=5)
        self.assertEqual(self.replies[-1], b'\x06' + struct.pack('<iI', -11, 96))
        self.assertEqual(self.commands, [])

    def test_empty_binary_write_is_rejected(self):
        self.feed(struct.pack('<I', 0), kind=5)
        self.assertEqual(self.writes, [])
        self.assertLess(struct.unpack_from('<i', self.replies[-1], 1)[0], 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    args = parser.parse_args()
    with args.elf.open('rb') as source:
        elf = ELFFile(source)
        SYMBOLS = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        SEGMENTS = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    HOOKS = {SYMBOLS[name] & ~1: name for name in ('apex_shell_generation', 'apex_shell_read',
             'apex_shell_send', 'shell_execute_cmd', 'z_impl_k_uptime_ticks')}
    for name in ('g4b_update_binary', 'apex_shell_bulk_touch'):
        if name in SYMBOLS:
            HOOKS[SYMBOLS[name] & ~1] = name
    unittest.main(argv=['radio-shell-tests'], verbosity=2)
