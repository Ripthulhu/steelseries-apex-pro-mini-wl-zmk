"""Execute the compiled installer against a small NVMC register model."""
from pathlib import Path
import struct
import json
import sys
from intelhex import IntelHex
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_LR

here = Path(__file__).parent
experiment = Path(sys.argv[1]) if len(sys.argv) > 1 else here
inspection = len(sys.argv) > 2 and sys.argv[2] == 'readback'
metadata = {'inspect_only': inspection, 'readback': inspection,
            'boot_input': str(experiment / 'bootloader_mbr.hex')}


def run(failure=None):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for address, size in ((0, 0x80000), (0x20000000, 0x20000), (0x10000000, 0x2000),
                          (0x40000000, 0x30000), (0xe000e000, 0x2000)):
        uc.mem_map(address, size)
    prefix = bytes((i * 13 + i // 4096) & 255 for i in range(0x23000))
    flash = bytearray(b'\xff' * 0x80000)
    flash[:len(prefix)] = prefix
    if metadata.get('backup'):
        flash = bytearray((Path(metadata['backup']) / 'flash-1.bin').read_bytes())
        prefix = bytes(flash[:0x23000])
    name = 'readback' if inspection else 'install'
    for offset, data in json.loads((experiment / (name + '.patch.json')).read_text())['edits']:
        data = bytes.fromhex(data)
        flash[0x23000 + offset:0x23000 + offset + len(data)] = data
    uc.mem_write(0, bytes(flash))
    uicr = bytearray(b'\xff' * 4096)
    struct.pack_into('<I', uicr, 0x304, 0)
    struct.pack_into('<I', uicr, 0x80, 0x12345678)
    struct.pack_into('<I', uicr, 0x208, 0)
    struct.pack_into('<II', uicr, 0x14, 0x74000, 0x7e000)
    if metadata.get('backup'):
        uicr = bytearray((Path(metadata['backup']) / 'uicr-1.bin').read_bytes())
    if failure == 'uicr':
        struct.pack_into('<I', uicr, 0x14, 0x70000)
    uc.mem_write(0x10001000, bytes(uicr))
    for address, value in ((0x10000010, 4096), (0x10000014, 128),
                           (0x10000100, 0x52833), (0x4001e400, 1)):
        uc.mem_write(address, struct.pack('<I', value))
    state = {'mode': 0, 'reset': False, 'fallback': False, 'erases': [], 'injected': False}

    def write(emu, access, address, size, value, _):
        if address == 0x4001e504:
            state['mode'] = value
        elif address == 0x4001e508:
            assert state['mode'] == 2 and value % 4096 == 0 and value < 0x80000
            state['erases'].append(value)
            emu.mem_write(value, b'\xff' * 4096)
        elif address == 0x4001e514:
            raise AssertionError('installer must not erase UICR')
        elif address < 0x80000 or 0x10001000 <= address < 0x10002000:
            assert state['mode'] == 1 and size == 4
            old = int.from_bytes(emu.mem_read(address, 4), 'little')
            assert address < 0x80000, 'installer must not program UICR'
            assert old & value == value, (hex(address), hex(old), hex(value))
        elif address == 0xe000ed0c:
            assert value == 0x05fa0004
            state['reset'] = True
            emu.emu_stop()

    def code(emu, address, size, _):
        if address == 0x23ad0:
            state['fallback'] = True
            emu.emu_stop()

    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    def read(emu, access, address, size, value, _):
        if (isinstance(failure, int) and address == failure and state['mode'] == 0
                and failure in state['erases'] and not state['injected']):
            word = int.from_bytes(emu.mem_read(address, 4), 'little')
            emu.mem_write(address, struct.pack('<I', word ^ 1))
            state['injected'] = True
    uc.hook_add(UC_HOOK_MEM_READ, read)
    uc.hook_add(UC_HOOK_CODE, code, begin=0x23ad0, end=0x23ad0)
    uc.reg_write(UC_ARM_REG_SP, 0x2000d000)
    uc.emu_start(0x26001, 0, timeout=20_000_000, count=10_000_000)
    if metadata.get('inspect_only'):
        assert state['fallback'] and not state['reset'] and not state['erases']
        assert bytes(uc.mem_read(0, 0x80000)) == flash
        assert bytes(uc.mem_read(0x10001000, 4096)) == uicr
        assert bytes(uc.mem_read(0x2001f800, 4)) == b'MIG1'
        print('PASS: inspection returns to stock startup without changing flash or UICR')
        if metadata.get('readback'):
            uc.hook_add(UC_HOOK_CODE, lambda emu, addr, size, data: emu.emu_stop(), begin=0x100, end=0x100)
            for offset in ((0, 56, 112, 128) if metadata.get('acl') else (0, 56, 0x7fff0, 0x80000, 0x80ff0, 0x81000)):
                uc.mem_write(0x2001f840, struct.pack('<I', offset))
                uc.reg_write(UC_ARM_REG_SP, 0x2000c000)
                uc.reg_write(UC_ARM_REG_R0, 0x20002000)
                uc.reg_write(UC_ARM_REG_LR, 0x101)
                uc.emu_start(0x24fa9, 0, timeout=1_000_000, count=10000)
                effective = offset if offset < 0x81000 else 0
                address = effective if effective < 0x80000 else 0x10001000 + effective - 0x80000
                limit = 0x80000 if effective < 0x80000 else 0x81000
                size = min(56, limit - effective)
                if metadata.get('acl'):
                    effective = offset if offset < 128 else 0
                    address = 0x4001e800 + effective
                    size = min(56, 128 - effective)
                expected = b'DMP1' + struct.pack('<I', address) + bytes(uc.mem_read(address, size)) + b'\xff' * (56 - size)
                assert bytes(uc.mem_read(0x20002000, 64)) == expected
            print('PASS: compiled readback wrapper, flash/UICR boundaries and wraparound')
        return
    assert bytes(uc.mem_read(0x10001000, 4096)) == uicr
    if failure in ('uicr', 0x74000, 0x7e000):
        assert state['fallback'] and not state['reset'] and 0 not in state['erases'], state
        assert bytes(uc.mem_read(0, len(prefix))) == prefix
        print('PASS: pre-commit failure returns to stock with low flash/UICR intact:', failure)
        return
    assert state['reset'], f"installer did not reset: {state}, pc={uc.reg_read(UC_ARM_REG_PC):x}"
    expected = IntelHex(metadata.get('boot_input', str(here / 'boot-artifacts/bootloader_mbr.hex')))
    assert bytes(uc.mem_read(0x74000, 0xa000)) == bytes(expected.tobinarray(start=0x74000, size=0xa000))
    assert bytes(uc.mem_read(0x1000, 0x22000)) == prefix[0x1000:]
    mbr = bytearray(expected.tobinarray(start=0, size=4096))
    struct.pack_into('<II', mbr, 0xff8, 0x74000, 0x7e000)
    assert bytes(uc.mem_read(0, 4096)) == mbr
    assert state['erases'][-1] == 0
    if failure == 0:
        assert state['erases'].count(0) == 2, state
    print('PASS: executed ARM installer; UICR/low flash preserved, loader verified, MBR last, reset; fault:', failure)


if __name__ == '__main__':
    run()
    if not metadata.get('inspect_only'):
        for fault in ('uicr', 0x74000, 0x7e000, 0):
            run(fault)
