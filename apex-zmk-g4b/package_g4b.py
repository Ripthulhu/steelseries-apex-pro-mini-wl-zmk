"""Package a G4A ZMK payload as a 307,200-byte SteelSeries update image.

The application begins at image offset 0. A recovery wrapper occupies one page
immediately above the application, the vendor-facing vectors point to that
wrapper, and the last word contains the image CRC-32. The expected application
vectors are supplied by the caller and verified before packaging.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path


IMAGE_BASE = 0x1C000
IMAGE_SIZE = 0x4B000
TRAILER_OFFSET = IMAGE_SIZE - 4
# The wrapper address must match FLASH_LOAD_OFFSET + FLASH_LOAD_SIZE. The
# default preserves the original 0x30000-byte application slot.
DEFAULT_FAILSAFE_ADDRESS = 0x4C000
# One-shot state is held in GPREGRET2; the old flash-cookie page is unused.
PAGE_SIZE = 0x1000
BOOTLOADER_ACCEPTED_SP = 0x20020000
CRC32_RESIDUE = 0x2144DF1C
WDT_RELOAD_MAGIC = struct.pack("<I", 0x6E524635)
SRAM_LOW = 0x20003000
SRAM_HIGH = 0x20020000


def parse_u32(text: str) -> int:
    return int(text, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("payload_bin", type=Path)
    parser.add_argument("failsafe_bin", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--expect-sp", type=parse_u32, required=True)
    parser.add_argument("--expect-reset", type=parse_u32, required=True)
    parser.add_argument("--wrapper-address", type=parse_u32,
                        default=DEFAULT_FAILSAFE_ADDRESS,
                        help="flash address of the RR7 wrapper; must equal "
                             "FLASH_LOAD_OFFSET + FLASH_LOAD_SIZE")
    args = parser.parse_args()

    payload = args.payload_bin.read_bytes()
    failsafe = args.failsafe_bin.read_bytes()

    failsafe_address = args.wrapper_address
    failsafe_offset = failsafe_address - IMAGE_BASE
    if failsafe_offset <= 0 or failsafe_offset + 0x1000 > IMAGE_SIZE:
        raise SystemExit(
            f"wrapper address 0x{failsafe_address:08x} does not leave a page "
            f"inside the image")
    if len(payload) < 8 or len(payload) > failsafe_offset:
        raise SystemExit(f"payload has an invalid size: {len(payload)}")
    if not failsafe or len(failsafe) > PAGE_SIZE:
        raise SystemExit(f"failsafe has an invalid size: {len(failsafe)}")
    # Multiple watchdog reload sites could bypass repeated-reset recovery.
    feeds = payload.count(WDT_RELOAD_MAGIC)
    if feeds > 1:
        raise SystemExit(
            f"payload contains the watchdog reload magic {feeds} times; there "
            f"must be exactly one feed site")
    # RR[7] is enforced by BUILD_ASSERT in wdt_g4b.c. Thumb movw/movt encoding
    # means its address is not present as a literal word in this binary.

    payload_sp, payload_reset = struct.unpack_from("<II", payload, 0)
    if (payload_sp, payload_reset) != (args.expect_sp, args.expect_reset):
        raise SystemExit(
            "payload vectors do not match the values extracted for this build: "
            f"sp=0x{payload_sp:08x} reset=0x{payload_reset:08x}"
        )
    if payload_reset & 1 != 1:
        raise SystemExit("payload reset vector is not an odd Thumb address")
    if not IMAGE_BASE <= (payload_reset & ~1) < IMAGE_BASE + len(payload):
        raise SystemExit("payload reset vector falls outside the payload")
    if not SRAM_LOW < payload_sp <= SRAM_HIGH:
        raise SystemExit("payload stack pointer falls outside the owned SRAM")
    if struct.pack("<I", payload_reset) not in failsafe:
        raise SystemExit("failsafe does not contain the exact payload reset vector")
    if struct.pack("<I", payload_sp) not in failsafe:
        raise SystemExit("failsafe does not contain the exact payload stack pointer")

    image = bytearray(IMAGE_SIZE)
    image[: len(payload)] = payload
    image[failsafe_offset : failsafe_offset + len(failsafe)] = failsafe
    struct.pack_into("<II", image, 0, BOOTLOADER_ACCEPTED_SP, failsafe_address | 1)

    crc = zlib.crc32(image[:-4]) & 0xFFFFFFFF
    struct.pack_into("<I", image, TRAILER_OFFSET, crc)
    if zlib.crc32(image) & 0xFFFFFFFF != CRC32_RESIDUE:
        raise AssertionError("unexpected output CRC residue")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"payload_size={len(payload)}")
    print(f"payload_end_address=0x{IMAGE_BASE + len(payload):08x}")
    print(f"payload_sp=0x{payload_sp:08x}")
    print(f"payload_reset=0x{payload_reset:08x}")
    print(f"payload_sha256={hashlib.sha256(payload).hexdigest()}")
    print(f"failsafe_size={len(failsafe)}")
    print(f"failsafe_sha256={hashlib.sha256(failsafe).hexdigest()}")
    print("one_shot=retained GPREGRET2 (no flash cookie)")
    print(f"trailer_crc32=0x{crc:08x}")
    print(f"output_sha256={hashlib.sha256(image).hexdigest()}")
    print(f"output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
