#!/usr/bin/env python3
"""Check the APEXBOOT self-update UF2 and its application staging space."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
BOOTLOADER_FAMILY_ID = 0xD663823C
APPLICATION_FAMILY_ID = 0x621E937A

MBR_END = 0x1000
BOOTLOADER_START = 0x74000
BOOTLOADER_END = 0x7E000
BOOTLOADER_SIZE = BOOTLOADER_END - BOOTLOADER_START
APP_UPDATE_END = 0x6D000
BOOTLOADER_STAGING_START = APP_UPDATE_END - BOOTLOADER_SIZE
UICR_BLOCK = 0x10001000
UICR_BOOTLOADER_ADDRESS = UICR_BLOCK + 0x14
UICR_MBR_PARAMS_ADDRESS = UICR_BLOCK + 0x18
BOARD_ID_KEY = 208
BOARD_ID_VALUE = 0x1D50616F


@dataclass(frozen=True)
class Block:
    number: int
    count: int
    flags: int
    family: int
    target: int
    payload: bytes


def read_uf2(path: Path) -> list[Block]:
    content = path.read_bytes()
    if not content or len(content) % 512:
        raise ValueError(f"{path.name} is not a non-empty UF2 file")

    blocks: list[Block] = []
    for file_number, offset in enumerate(range(0, len(content), 512)):
        raw = content[offset : offset + 512]
        magic0, magic1, flags, target, payload_size, number, count, family = \
            struct.unpack_from("<8I", raw)
        if (magic0, magic1, struct.unpack_from("<I", raw, 508)[0]) != (
            UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END
        ):
            raise ValueError(f"invalid UF2 magic in file block {file_number}")
        if flags & UF2_FLAG_FAMILY_ID == 0:
            raise ValueError(f"UF2 block {number} has no family ID")
        if payload_size == 0 or payload_size > 476:
            raise ValueError(f"invalid payload size in UF2 block {number}")
        blocks.append(Block(number, count, flags, family, target,
                            raw[32 : 32 + payload_size]))

    expected_count = len(blocks)
    if any(block.count != expected_count for block in blocks):
        raise ValueError("UF2 block count is inconsistent")
    numbers = [block.number for block in blocks]
    if len(set(numbers)) != expected_count or set(numbers) != set(range(expected_count)):
        raise ValueError("UF2 block sequence is incomplete or duplicated")
    return blocks


def memory_from_blocks(blocks: list[Block]) -> dict[int, int]:
    memory: dict[int, int] = {}
    for block in blocks:
        for offset, value in enumerate(block.payload):
            address = block.target + offset
            if address in memory and memory[address] != value:
                raise ValueError(f"conflicting UF2 data at 0x{address:x}")
            memory[address] = value
    return memory


def read_word(memory: dict[int, int], address: int) -> int:
    try:
        value = bytes(memory[address + offset] for offset in range(4))
    except KeyError as error:
        raise ValueError(f"UF2 is missing data at 0x{address:x}") from error
    return struct.unpack("<I", value)[0]


def verify(update_path: Path, application_path: Path) -> None:
    update_blocks = read_uf2(update_path)
    if any(block.family != BOOTLOADER_FAMILY_ID for block in update_blocks):
        raise ValueError("bootloader updater has the wrong UF2 family ID")

    has_mbr = False
    has_bootloader = False
    uicr_blocks = 0
    for block in update_blocks:
        start = block.target
        end = start + len(block.payload)
        if start < MBR_END and end <= MBR_END:
            has_mbr = True
        elif BOOTLOADER_START <= start and end <= BOOTLOADER_END:
            has_bootloader = True
        elif start == UICR_BLOCK and end <= UICR_BLOCK + 0x1000:
            uicr_blocks += 1
        else:
            raise ValueError(
                f"bootloader UF2 block {block.number} targets unexpected range "
                f"0x{start:x}..0x{end:x}"
            )
    if not has_mbr or not has_bootloader or uicr_blocks != 1:
        raise ValueError("bootloader updater must contain MBR, bootloader, and one UICR block")

    update_memory = memory_from_blocks(update_blocks)
    if read_word(update_memory, UICR_BOOTLOADER_ADDRESS) != BOOTLOADER_START:
        raise ValueError("bootloader updater has the wrong UICR bootloader address")
    if read_word(update_memory, UICR_MBR_PARAMS_ADDRESS) != BOOTLOADER_END:
        raise ValueError("bootloader updater has the wrong UICR MBR parameters address")

    board_id = struct.pack("<II", BOARD_ID_KEY, BOARD_ID_VALUE)
    bootloader_data = bytes(
        update_memory.get(address, 0xFF)
        for address in range(BOOTLOADER_START, BOOTLOADER_END)
    )
    if board_id not in bootloader_data:
        raise ValueError("bootloader updater does not contain the Apex board ID")

    application_blocks = read_uf2(application_path)
    if any(block.family != APPLICATION_FAMILY_ID for block in application_blocks):
        raise ValueError("application has the wrong UF2 family ID")
    application_end = max(
        block.target + len(block.payload) for block in application_blocks
    )
    if application_end > BOOTLOADER_STAGING_START:
        raise ValueError(
            f"application ends at 0x{application_end:x}, but bootloader updates "
            f"need 0x{BOOTLOADER_STAGING_START:x}..0x{APP_UPDATE_END:x} for staging"
        )

    print(f"bootloader_update={update_path}")
    print(f"blocks={len(update_blocks)} family=0x{BOOTLOADER_FAMILY_ID:08x}")
    print(
        f"staging=0x{BOOTLOADER_STAGING_START:x}..0x{APP_UPDATE_END:x} "
        f"application_end=0x{application_end:x}"
    )
    print("bootloader_update_verification=PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bootloader_update", type=Path)
    parser.add_argument("application", type=Path)
    args = parser.parse_args()
    verify(args.bootloader_update, args.application)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
