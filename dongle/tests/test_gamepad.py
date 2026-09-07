"""Run the receiver's compiled gamepad code with mocked USB and kernel calls.

Requires pyelftools and Unicorn. No connected device is accessed.
"""
import argparse
from pathlib import Path
import struct
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                              UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

NEUTRAL = struct.pack('<5HB', 16384, 16384, 0, 0, 16384, 0)
DATA = 0x2001d000


class GamepadTests(unittest.TestCase):
    def setUp(self):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0, 0x80000)
        self.cpu.mem_map(0x20000000, 0x20000)
        for address, data in SEGMENTS:
            self.cpu.mem_write(address, data)
        self.now = 0
        self.submit_result = 0
        self.sent = []
        self.targets = []
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)
        self.assertEqual(self.call('receiver_gamepad_init'), 0)
        self.call('iface_ready', 0, 1)

    def hook(self, cpu, address, size, user):
        if address == 0x100:
            self.returned = True
            cpu.emu_stop()
            return
        name = HOOKS.get(address)
        if not name:
            return
        if getattr(self, 'usb_test', False):
            if name == 'receiver_usb_gamepad':
                return
            if name.startswith('usbd_'):
                self.usb_calls.append(name)
                self.assertNotEqual(name, 'usbd_unregister_all_classes',
                                    'shutdown already removed the classes')
                if name == 'usbd_shutdown':
                    self.strings = 0
                elif name == 'usbd_add_descriptor':
                    self.strings += 1
                elif name == 'usbd_init':
                    self.assertEqual(self.strings, 4, 'device strings must be restored')
                cpu.reg_write(UC_ARM_REG_R0, 0)
                cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))
                return
        result = 0
        if name in ('z_spin_lock_valid', 'z_spin_unlock_valid'):
            result = 1
        elif name == 'z_impl_k_uptime_ticks':
            result = (self.now * 32768 + 999) // 1000
            cpu.reg_write(UC_ARM_REG_R1, 0)
        elif name == 'receiver_usb_gamepad':
            self.targets.append(cpu.reg_read(UC_ARM_REG_R0))
        elif name == 'hid_device_submit_report':
            self.sent.append(bytes(cpu.mem_read(cpu.reg_read(UC_ARM_REG_R2), 11)))
            result = self.submit_result
        cpu.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def call(self, name, *args):
        self.returned = False
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.cpu.reg_write(reg, value & 0xffffffff)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001e000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.cpu.emu_start(SYMBOLS[name] | 1, 0, timeout=1_000_000, count=100000)
        self.assertTrue(self.returned, name)
        return self.cpu.reg_read(UC_ARM_REG_R0)

    def receive(self, value, enabled=1):
        report = struct.pack('<5HB', value, value, value, value, value, 0)
        self.cpu.mem_write(DATA, report)
        self.call('apex_radio_gamepad_receive', enabled, DATA)
        return report

    def complete(self, status=0):
        self.call('report_done', 0, 0, status)

    def test_latest_state_replaces_pending_axes(self):
        first = self.receive(100)
        self.call('receiver_gamepad_poll')
        self.receive(200)
        latest = self.receive(300)
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent, [first])
        self.complete()
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent, [first, latest])

    def test_link_loss_clears_pending_axes(self):
        self.receive(32767)
        self.call('receiver_gamepad_poll')
        self.call('receiver_gamepad_release')
        self.complete()
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent[-1], NEUTRAL)

    def test_timeout_without_link_loss(self):
        self.receive(32767)
        self.call('receiver_gamepad_poll')
        self.complete()
        self.now = 99
        self.call('receiver_gamepad_poll')
        self.assertEqual(len(self.sent), 1)
        self.now = 100
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent[-1], NEUTRAL)

    def test_usb_failure_retries_newest_state(self):
        self.submit_result = -11
        self.receive(100)
        self.call('receiver_gamepad_poll')
        latest = self.receive(200)
        self.submit_result = 0
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent[-1], latest)
        self.complete(-125)
        latest = self.receive(300)
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent[-1], latest)

    def test_disabled_and_unconfigured(self):
        self.receive(32767, enabled=0)
        self.assertEqual(self.targets[-1], 0)
        self.call('receiver_gamepad_poll')
        self.assertEqual(self.sent[-1], NEUTRAL)
        self.complete()
        self.call('iface_ready', 0, 0)
        self.receive(100)
        self.call('receiver_gamepad_poll')
        self.assertEqual(len(self.sent), 1)

    def test_usb_reconfiguration_restores_descriptors(self):
        self.usb_test = True
        self.usb_calls = []
        self.strings = 0
        self.assertEqual(self.call('receiver_usb_init'), 0)
        initializations = 1
        for enabled in (1, 0, 1, 0):
            self.call('receiver_usb_gamepad', enabled)
            self.call('usb_reconfigure', DATA)
            initializations += 1
            self.assertEqual(self.usb_calls.count('usbd_init'), initializations)
            self.assertEqual(self.usb_calls[-1], 'usbd_enable')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    args = parser.parse_args()
    with args.elf.open('rb') as source:
        elf = ELFFile(source)
        symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
        SYMBOLS = {s.name: int(s['st_value']) for s in symbols if s['st_info']['bind'] == 'STB_GLOBAL'}
        current_file = None
        for symbol in symbols:
            if symbol['st_info']['type'] == 'STT_FILE':
                current_file = symbol.name
            elif current_file == 'gamepad.c':
                SYMBOLS[symbol.name] = int(symbol['st_value'])
            elif current_file == 'usb.c':
                SYMBOLS['usb_' + symbol.name] = int(symbol['st_value'])
        SEGMENTS = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD' and s['p_filesz']]
    HOOKS = {SYMBOLS[name] & ~1: name for name in (
        'z_spin_lock_valid', 'z_spin_unlock_valid', 'z_spin_lock_set_owner',
        'z_impl_k_uptime_ticks', 'receiver_usb_gamepad',
        'hid_device_register', 'hid_device_submit_report', 'k_work_submit_to_queue',
        'usbd_add_descriptor', 'usbd_add_configuration', 'usbd_register_all_classes',
        'usbd_unregister_all_classes', 'usbd_init', 'usbd_enable', 'usbd_disable',
        'usbd_shutdown', 'usbd_device_set_code_triple', 'usbd_msg_register_cb') if name in SYMBOLS}
    unittest.main(argv=['gamepad-tests'], verbosity=2)
