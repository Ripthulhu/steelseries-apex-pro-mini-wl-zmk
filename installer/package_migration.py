# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path


IMAGE_BASE = 0x1C000
IMAGE_SIZE = 0x4B000
METADATA_ADDRESS = 0x5B000
BOOTLOADER_SOURCE = 0x5C000
BOOTLOADER_DESTINATION = 0x6E000
BOOTLOADER_END = 0x78000
BOOTLOADER_LENGTH = BOOTLOADER_END - BOOTLOADER_DESTINATION
INTERNAL_FLASH_END = 0x80000
UICR_BOOTLOADER_ADDRESS = 0x10001014
UICR_PARAM_PAGE_ADDRESS = 0x10001018
CRC32_RESIDUE = 0x2144DF1C
METADATA = struct.Struct("<8s6I")


def read_ihex(path: Path) -> dict[int, int]:
    memory: dict[int, int] = {}
    upper = 0
    saw_eof = False
    for line_number, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"{path}:{line_number}: invalid Intel HEX record")
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or sum(record) & 0xFF:
            raise ValueError(f"{path}:{line_number}: invalid Intel HEX checksum")
        count, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        data = record[4:-1]
        if count != len(data):
            raise ValueError(f"{path}:{line_number}: invalid byte count")
        if kind == 0:
            absolute = upper + address
            for offset, value in enumerate(data):
                target = absolute + offset
                if target in memory and memory[target] != value:
                    raise ValueError(
                        f"{path}:{line_number}: conflicting byte at 0x{target:08x}"
                    )
                memory[target] = value
        elif kind == 1:
            if count != 0 or address != 0:
                raise ValueError(f"{path}:{line_number}: malformed EOF record")
            saw_eof = True
            break
        elif kind == 2:
            if count != 2 or address != 0:
                raise ValueError(f"{path}:{line_number}: malformed segment-address record")
            upper = int.from_bytes(data, "big") << 4
        elif kind == 4:
            if count != 2 or address != 0:
                raise ValueError(f"{path}:{line_number}: malformed linear-address record")
            upper = int.from_bytes(data, "big") << 16
        elif kind in (3, 5):
            if count != 4 or address != 0:
                raise ValueError(f"{path}:{line_number}: malformed start-address record")
        else:
            raise ValueError(f"{path}:{line_number}: unsupported record type {kind}")
    if not saw_eof:
        raise ValueError(f"{path}: missing EOF record")
    return memory


def extract_bootloader(path: Path) -> bytes:
    memory = read_ihex(path)
    allowed_internal = (
        range(0x6B000, 0x6D000),
        range(BOOTLOADER_DESTINATION, BOOTLOADER_END),
    )
    unexpected = [
        address for address, value in memory.items()
        if address < INTERNAL_FLASH_END and value != 0xFF and
        not any(address in allowed for allowed in allowed_internal)
    ]
    if unexpected:
        first = min(unexpected)
        raise ValueError(
            "bootloader HEX contains data outside the legacy layout: "
            f"0x{first:08x}"
        )
    for address, expected in (
        (UICR_BOOTLOADER_ADDRESS, BOOTLOADER_DESTINATION),
        (UICR_PARAM_PAGE_ADDRESS, 0x6B000),
    ):
        raw = bytes(memory.get(address + offset, 0xFF) for offset in range(4))
        actual = int.from_bytes(raw, "little")
        if actual != expected:
            raise ValueError(
                f"bootloader HEX has wrong UICR marker at 0x{address:08x}: "
                f"0x{actual:08x}"
            )
    image = bytes(memory.get(address, 0xFF) for address in range(
        BOOTLOADER_DESTINATION, BOOTLOADER_END
    ))
    stack, reset = struct.unpack_from("<II", image)
    if not 0x20000000 <= stack <= 0x20020000:
        raise ValueError(f"bootloader stack is invalid: 0x{stack:08x}")
    if reset & 1 == 0 or not BOOTLOADER_DESTINATION <= (reset & ~1) < BOOTLOADER_END:
        raise ValueError(f"bootloader reset vector is invalid: 0x{reset:08x}")
    if image == b"\xff" * len(image):
        raise ValueError("bootloader region is empty")
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the stock-recovery migration image")
    parser.add_argument("migration_bin", type=Path)
    parser.add_argument("bootloader_hex", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    migration = args.migration_bin.read_bytes()
    if len(migration) < 0x1008 or len(migration) > METADATA_ADDRESS - IMAGE_BASE:
        raise SystemExit(f"migration code has invalid size: {len(migration)}")
    stack, reset = struct.unpack_from("<II", migration)
    if stack != 0x20020000 or not IMAGE_BASE <= (reset & ~1) < METADATA_ADDRESS:
        raise SystemExit(f"migration vectors are invalid: 0x{stack:08x}, 0x{reset:08x}")

    bootloader = extract_bootloader(args.bootloader_hex)
    bootloader_crc = zlib.crc32(bootloader) & 0xFFFFFFFF
    metadata = METADATA.pack(
        b"APXMIG1\0", 1, BOOTLOADER_SOURCE, BOOTLOADER_DESTINATION,
        BOOTLOADER_LENGTH, bootloader_crc, 0,
    )

    image = bytearray(IMAGE_SIZE)
    image[: len(migration)] = migration
    metadata_offset = METADATA_ADDRESS - IMAGE_BASE
    source_offset = BOOTLOADER_SOURCE - IMAGE_BASE
    image[metadata_offset : metadata_offset + len(metadata)] = metadata
    image[source_offset : source_offset + len(bootloader)] = bootloader
    trailer_crc = zlib.crc32(image[:-4]) & 0xFFFFFFFF
    struct.pack_into("<I", image, IMAGE_SIZE - 4, trailer_crc)
    if zlib.crc32(image) & 0xFFFFFFFF != CRC32_RESIDUE:
        raise AssertionError("vendor image CRC residue is wrong")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"migration_size={len(migration)}")
    print(f"bootloader_size={len(bootloader)}")
    print(f"bootloader_crc32=0x{bootloader_crc:08x}")
    print(f"bootloader_sha256={hashlib.sha256(bootloader).hexdigest()}")
    print(f"trailer_crc32=0x{trailer_crc:08x}")
    print(f"output_sha256={hashlib.sha256(image).hexdigest()}")
    print(f"output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
