#!/usr/bin/env python3
"""Create the reviewed nRF52833 UICR image used by the open bootloader."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


UICR_SIZE = 0x1000
WORDS = {
    0x014: 0x00074000,  # NRFFW[0]: bootloader start
    0x018: 0x0007E000,  # NRFFW[1]: MBR parameter page
    0x200: 0x00000012,  # PSELRESET[0]: P0.18
    0x204: 0x00000012,  # PSELRESET[1]: P0.18
    0x208: 0xFFFFFFFF,  # APPROTECT: debug access open
    0x20C: 0xFFFFFFFE,  # NFCPINS: P0.09/P0.10 are GPIO
}


def build_image() -> bytes:
    image = bytearray(b"\xff" * UICR_SIZE)
    for offset, value in WORDS.items():
        struct.pack_into("<I", image, offset, value)
    return bytes(image)


def validate(image: bytes) -> None:
    if len(image) != UICR_SIZE:
        raise ValueError(f"UICR image is {len(image)} bytes, expected {UICR_SIZE}")
    for offset, expected in WORDS.items():
        actual = struct.unpack_from("<I", image, offset)[0]
        if actual != expected:
            raise ValueError(
                f"UICR word at +0x{offset:03x} is 0x{actual:08x}, "
                f"expected 0x{expected:08x}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not args.check and args.output is None:
        parser.error("provide an output path or use --check")

    image = build_image()
    validate(image)
    digest = hashlib.sha256(image).hexdigest()
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
        print(f"wrote={args.output}")
    print(f"size={len(image)}")
    print(f"sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
