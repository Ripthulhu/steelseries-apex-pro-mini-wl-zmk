"""Test command forwarding in the receiver ELF without accessing a device."""
import argparse
from pathlib import Path
import struct
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)


class KeyboardShellTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0, 0x80000)
        self.cpu.mem_map(0x20000000, 0x20000)
        for address, data in SEGMENTS:
            self.cpu.mem_write(address, data)
        self.frames = []
        self.connected = True
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
        result = 0
        if name == 'apex_radio_input_connected':
            result = int(self.connected)
        elif name == 'apex_shell_generation':
            result = 1
        elif name == 'z_impl_k_uptime_ticks':
            cpu.reg_write(UC_ARM_REG_R1, 0)
        elif name == 'apex_shell_send':
            self.frames.append(bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R1),
                                                 cpu.reg_read(UC_ARM_REG_R2))))
        elif name == 'apex_shell_read':
            cpu.mem_write(cpu.reg_read(UC_ARM_REG_R1), b'\x04\0\0\0\0')
            result = 5
        cpu.reg_write(UC_ARM_REG_R0, result)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def run_command(self, *args):
        pointers = []
        position = 0x2001c000
        for arg in ('keyboard', *args):
            data = arg.encode() + b'\0'
            pointers.append(position)
            self.cpu.mem_write(position, data)
            position += len(data)
        self.cpu.mem_write(0x2001d000, struct.pack('<' + 'I' * len(pointers), *pointers))
        self.cpu.reg_write(UC_ARM_REG_R0, 0)
        self.cpu.reg_write(UC_ARM_REG_R1, len(pointers))
        self.cpu.reg_write(UC_ARM_REG_R2, 0x2001d000)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001e000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.cpu.emu_start(SYMBOLS['receiver_keyboard_command'] | 1, 0,
                           timeout=1_000_000, count=100000)
        self.assertTrue(self.returned)
        return self.cpu.reg_read(UC_ARM_REG_R0)

    def test_battery(self):
        self.assertEqual(self.run_command('battery'), 0)
        self.assertEqual(self.frames, [b'\x02apex battery'])

    def test_radio_name(self):
        self.assertEqual(self.run_command('radio'), 0)
        self.assertEqual(self.frames, [b'\x02apex radio_test'])

    def test_settings_arguments(self):
        self.assertEqual(self.run_command('rt', 'on', '0.3'), 0)
        self.assertEqual(self.frames, [b'\x02apex rt on 0.3'])

    def test_help_without_connection(self):
        self.connected = False
        self.assertEqual(self.run_command(), 0)
        self.assertEqual(self.frames, [])

    def test_disconnected(self):
        self.connected = False
        self.assertNotEqual(self.run_command('battery'), 0)
        self.assertEqual(self.frames, [])

    def test_exact_limit_fragments(self):
        self.assertEqual(self.run_command('x' * 122), 0)
        self.assertEqual(b''.join(f[1:] for f in self.frames), b'apex ' + b'x' * 122)
        self.assertEqual([f[0] for f in self.frames], [1, 1, 2])

    def test_oversized_command(self):
        self.assertNotEqual(self.run_command('x' * 123), 0)
        self.assertEqual(self.frames, [])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    args = parser.parse_args()
    with args.elf.open('rb') as source:
        elf = ELFFile(source)
        SYMBOLS = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        SEGMENTS = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    HOOKS = {SYMBOLS[name] & ~1: name for name in (
        'apex_radio_input_connected', 'apex_shell_generation', 'apex_shell_send',
        'apex_shell_read', 'z_impl_k_uptime_ticks', 'shell_fprintf_impl',
        'shell_fprintf_normal', 'shell_fprintf_error')}
    unittest.main(argv=['keyboard-shell-tests'], verbosity=2)
