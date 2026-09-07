"""Exercise the compiled UF2 parser with flash calls recorded instead of executed."""
import json
from pathlib import Path
import struct
import sys
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

folder = Path(sys.argv[1]).resolve()
with (folder / 'bootloader.elf').open('rb') as stream:
    elf = ELFFile(stream)
    symbols = {s.name: int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
    segments = [(int(p['p_vaddr']), p.data()) for p in elf.iter_segments()
                if p['p_type'] == 'PT_LOAD' and p['p_filesz']]

assert 'ble_stack_init' not in symbols, 'BLE update code is still linked'
assert 'board_rgb_progress' not in symbols
assert 'led_pwm_init' not in symbols, 'unused LED initialization is still linked'

def check(name, target=0x1000, family=0x1d506170, length=256, flags=0x2000, magic=0x0a324655,
          expected=512, written=True):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for start, size in ((0, 0x80000), (0x20000000, 0x20000), (0x10000000, 0x2000)):
        uc.mem_map(start, size)
    for start, data in segments:
        uc.mem_write(start, data)
    data = bytearray(512)
    struct.pack_into('<8I', data, 0, magic, 0x9e5d5157, flags, target, length, 0, 1, family)
    data[32:288] = bytes(range(256))
    struct.pack_into('<I', data, 508, 0x0ab16f30)
    uc.mem_write(0x20001000, bytes(data))
    calls = []
    returned = []
    def hook(emu, address, size, _):
        if address == (symbols['flash_nrf5x_write'] & ~1):
            calls.append((emu.reg_read(UC_ARM_REG_R0), emu.reg_read(UC_ARM_REG_R2)))
            emu.reg_write(UC_ARM_REG_PC, emu.reg_read(UC_ARM_REG_LR))
        elif address == (symbols['flash_nrf5x_flush'] & ~1):
            emu.reg_write(UC_ARM_REG_PC, emu.reg_read(UC_ARM_REG_LR))
        elif address == 0x100:
            returned.append(emu.reg_read(UC_ARM_REG_R0))
            emu.emu_stop()
    def memory(emu, access, address, size, value, _):
        assert 0x20000000 <= address < 0x20020000, ('unexpected write', hex(address))
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.hook_add(UC_HOOK_MEM_WRITE, memory)
    for reg, value in ((UC_ARM_REG_SP, 0x2001f000), (UC_ARM_REG_LR, 0x101),
                       (UC_ARM_REG_R0, 0), (UC_ARM_REG_R1, 0x20001000), (UC_ARM_REG_R2, 0x20002000)):
        uc.reg_write(reg, value)
    uc.emu_start(symbols['write_block'] | 1, 0, timeout=1_000_000, count=100000)
    assert returned == [expected & 0xffffffff], (name, returned, hex(uc.reg_read(UC_ARM_REG_PC)))
    assert calls == ([(target, length)] if written else []), (name, calls)
    print('PASS:', name)

check('first application block')
check('last application block', target=0x6cf00)
check('generic nRF52833 image rejected', family=0x621e937a, expected=-1, written=False)
check('keyboard board image rejected', family=0x1d50616f, expected=-1, written=False)
check('reserved pages rejected', target=0x6d000, expected=-1, written=False)
check('bootloader address rejected as application', target=0x74000, expected=-1, written=False)
check('UICR rejected as application', target=0x10001000, expected=-1, written=False)
check('MBR skipped without writing', target=0, written=False)
check('unaligned block rejected', target=0x1001, expected=-1, written=False)
check('oversized block rejected', length=512, expected=-1, written=False)
check('missing family flag rejected', flags=0, expected=-1, written=False)
check('bad magic rejected', magic=0, expected=-1, written=False)

def serial_check(name, revision=0x6170, extension=2, sd_count=1, image_type=4, expected=0, short=False):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for start, size in ((0, 0x80000), (0x20000000, 0x20000), (0x10000000, 0x2000)):
        uc.mem_map(start, size)
    for start, data in segments:
        uc.mem_write(start, data)
    packet = struct.pack('<HHIHH', 0x52, revision, 1, sd_count, 0xfffe) + bytes(extension)
    if short:
        packet = packet[:10]
    uc.mem_write(0x20001000, packet)
    returned = []
    def code(emu, address, size, _):
        if address == 0x100:
            returned.append(emu.reg_read(UC_ARM_REG_R0))
            emu.emu_stop()
    def memory(emu, access, address, size, value, _):
        assert (0x2001e000 <= address < 0x2001f000 or
                symbols['m_extended_packet'] <= address and address + size <= symbols['m_extended_packet'] + 104), hex(address)
    uc.hook_add(UC_HOOK_CODE, code)
    uc.hook_add(UC_HOOK_MEM_WRITE, memory)
    for reg, value in ((UC_ARM_REG_SP, 0x2001f000), (UC_ARM_REG_LR, 0x101),
                       (UC_ARM_REG_R0, 0x20001000), (UC_ARM_REG_R1, len(packet)), (UC_ARM_REG_R2, image_type)):
        uc.reg_write(reg, value)
    uc.emu_start(symbols['dfu_init_prevalidate'] | 1, 0, timeout=1_000_000, count=100000)
    assert returned == [expected], (name, returned)
    print('PASS:', name)

serial_check('dongle serial app accepted')
serial_check('dongle serial bootloader accepted', image_type=2)
serial_check('generic serial application rejected', revision=52833, expected=15)
serial_check('generic serial bootloader rejected', revision=52833, image_type=2, expected=15)
serial_check('serial short header rejected', short=True, expected=9)
serial_check('serial missing checksum rejected', extension=1, expected=9)
serial_check('serial maximum extension accepted', extension=104)
serial_check('serial oversized extension rejected', extension=105, expected=9)
serial_check('serial length truncation rejected', extension=258, expected=9)
serial_check('serial device-list overrun rejected', sd_count=65535, expected=9)
(folder / 'parser-tests.json').write_text(json.dumps({'uf2_passed': 12, 'serial_passed': 10,
    'method': 'compiled ARM parsers; flash calls mocked; serial destination bounds checked'}, indent=2) + '\n')

