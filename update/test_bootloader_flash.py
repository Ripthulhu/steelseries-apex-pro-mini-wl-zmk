"""Run the compiled bootloader install path against a faithful NVM flash model.

Unlike test_bootloader.py, this harness does NOT stub flash_nrf5x_write() and
flash_nrf5x_flush(). It lets the real Adafruit page-cache code execute in the
emulator and models the flash only at the two hardware primitives it ends up
calling: nrfx_nvmc_page_erase() and nrfx_nvmc_words_write().

The model enforces the two properties the old stub ignored:

  1. A program can only clear bits (1 -> 0). Writing the same value back is
     always legal and never sets a bit, so it raises no violation here.
  2. There is a hardware limit on how many times a word may be programmed
     between erases. This harness counts programs per word since the last erase
     of that word's page and reports the maximum.

The nRF52 NVMC does not enforce (2); exceeding it produces cells that read back
correctly right after programming but do not retain reliably. That matches the
observed hardware failure: the changed image verified at the end of install
(fresh cells read correct), then after a power cycle the trial image was
corrupt and the keyboard reported it REJECTED.

Run:
    python update/test_bootloader_flash.py /path/to/bootloader.elf
"""
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

PAGE = 0x1000

# Programs allowed to one word between erases. The nRF52832 product
# specification documents 2; the newer nRF52833/nRF52840 flash allows a small
# number but is still single digit. The install path must stay at 1: every
# destination word is erased once and then programmed once.
WRITE_LIMIT = 2


class FlashModelTests(unittest.TestCase):
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
        # Internal-flash program accounting, reset per page erase.
        self.write_count = {}     # word address -> programs since last erase
        self.max_write = 0        # worst word seen this run
        self.bit_set_violations = []  # addresses where a 0->1 program was attempted
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    # --- emulated hardware -------------------------------------------------
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
        elif name == 'nrfx_nvmc_page_erase':
            page = a & ~(PAGE - 1)
            self.erased.append(page)
            cpu.mem_write(page, b'\xff' * PAGE)
            for word in range(page, page + PAGE, 4):
                self.write_count.pop(word, None)
        elif name == 'nrfx_nvmc_words_write':
            # R0 = dst word address, R1 = src, R2 = number of 32-bit words.
            for i in range(n):
                dst = a + i * 4
                old = int.from_bytes(cpu.mem_read(dst, 4), 'little')
                new = int.from_bytes(cpu.mem_read(pointer + i * 4, 4), 'little')
                if new & ~old:
                    self.bit_set_violations.append(dst)
                cpu.mem_write(dst, (old & new).to_bytes(4, 'little'))
                count = self.write_count.get(dst, 0) + 1
                self.write_count[dst] = count
                if count > self.max_write:
                    self.max_write = count
        # nrfx_nvmc_page_erase and nrfx_nvmc_words_write return void; the reads
        # (nor_read) set result above. Return to the caller.
        cpu.reg_write(UC_ARM_REG_R0, result)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    # --- driver ------------------------------------------------------------
    def run_boot(self):
        self.returned = False
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2001f000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x101)
        self.cpu.emu_start(SYMBOLS['ab_promote_check'] | 1, 0,
                           timeout=120_000_000, count=60_000_000)
        self.assertTrue(self.returned)

    def image(self, value, pages=2):
        words = pages * PAGE
        image = bytearray([value] * words)
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

    # --- tests -------------------------------------------------------------
    def test_changed_image_programs_each_word_once(self):
        """The changed-image copy must not program any word more than the flash
        allows between erases. The current adapter flushes the whole 4 KiB page
        cache after every 256-byte write, so the first words of every page are
        reprogrammed up to 16 times; this asserts that regression away."""
        old, new = self.image(0x11), self.image(0x22)
        self.fallback(old)
        self.candidate(new)
        self.cpu.mem_write(0x1000, old)   # internal starts as the old build
        self.run_boot()

        # Install completed: state advanced to TRIAL and the copy landed.
        self.assertEqual(struct.unpack_from('<I', self.nor, 0x82034)[0], 0xfffffff8)
        self.assertEqual(bytes(self.cpu.mem_read(0x1000, len(new))), new)
        self.assertEqual(self.bit_set_violations, [])
        self.assertLessEqual(
            self.max_write, WRITE_LIMIT,
            f"a word was programmed {self.max_write} times between erases; "
            f"the nRF52 NVMC allows at most {WRITE_LIMIT}. The full image "
            f"verified here but will not retain across a reboot.")

    def test_same_image_writes_nothing(self):
        """Reinstalling the identical image erases and programs no internal
        page (the per-page 'same' short-circuit), which is why the same-image
        install passed on hardware while the changed image did not."""
        same = self.image(0x33)
        self.fallback(self.image(0x11))
        self.candidate(same)
        self.cpu.mem_write(0x1000, same)
        self.run_boot()
        self.assertEqual(struct.unpack_from('<I', self.nor, 0x82034)[0], 0xfffffff8)
        # No application page is erased or programmed. app_finish() still clears
        # the bootloader settings page (0x7f000); that is expected bookkeeping.
        app_erased = [p for p in self.erased if 0x1000 <= p < 0x72000]
        self.assertEqual(app_erased, [])
        self.assertEqual(self.max_write, 0)


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
        'nor_read', 'nor_close', 'update_nor_write',
        'nrfx_nvmc_page_erase', 'nrfx_nvmc_words_write')}
    # No-op the RGB install-progress bar (drives SPIM2 on real hardware).
    for opt in ('update_nor_wait', 'board_rgb_progress'):
        if opt in SYMBOLS:
            HOOKS[SYMBOLS[opt] & ~1] = opt
    unittest.main(argv=['boot-update-flash-tests'], verbosity=2)
