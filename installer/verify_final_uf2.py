# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
APEX_FAMILY_ID = 0x621E937A
APP_START = 0x1000
APP_END = 0x67000
SOFTDEVICE_MAGIC_ADDRESS = 0x3004
SOFTDEVICE_MAGIC = struct.pack("<I", 0x51B1E5DB)


def verify(path: Path, expected_sha256: str | None = None) -> None:
    content = path.read_bytes()
    digest = hashlib.sha256(content).hexdigest()
    if expected_sha256 is not None and digest.lower() != expected_sha256.lower():
        raise ValueError(f"UF2 SHA-256 mismatch: got {digest}")
    if not content or len(content) % 512:
        raise ValueError("UF2 size is not a non-zero multiple of 512")

    seen: dict[int, tuple[int, bytes]] = {}
    declared_blocks: int | None = None
    memory: dict[int, int] = {}
    for offset in range(0, len(content), 512):
        block = content[offset : offset + 512]
        magic0, magic1, flags, target, payload_size, block_no, block_count, family = \
            struct.unpack_from("<8I", block)
        if (magic0, magic1, struct.unpack_from("<I", block, 508)[0]) != (
            UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END
        ):
            raise ValueError(f"invalid UF2 magic in file block {offset // 512}")
        if flags & UF2_FLAG_FAMILY_ID == 0 or family != APEX_FAMILY_ID:
            raise ValueError(f"wrong or missing UF2 family in block {block_no}")
        if payload_size == 0 or payload_size > 476:
            raise ValueError(f"invalid payload size in block {block_no}: {payload_size}")
        if target < APP_START or target + payload_size > APP_END:
            raise ValueError(f"block {block_no} escapes 0x{APP_START:x}..0x{APP_END:x}")
        if block_no in seen:
            raise ValueError(f"duplicate UF2 block number: {block_no}")
        if declared_blocks is None:
            declared_blocks = block_count
        elif block_count != declared_blocks:
            raise ValueError("inconsistent UF2 block count")
        payload = block[32 : 32 + payload_size]
        seen[block_no] = (target, payload)
        for index, value in enumerate(payload):
            address = target + index
            if address in memory and memory[address] != value:
                raise ValueError(f"conflicting payload at 0x{address:x}")
            memory[address] = value

    if declared_blocks != len(seen) or set(seen) != set(range(declared_blocks or 0)):
        raise ValueError("UF2 block sequence is incomplete")
    if any(address not in memory for address in range(APP_START, APP_START + 8)):
        raise ValueError("UF2 does not contain the complete application vector")
    vector = bytes(memory[address] for address in range(APP_START, APP_START + 8))
    stack, reset = struct.unpack("<II", vector)
    if not 0x20000000 <= stack <= 0x20020000 or reset & 1 == 0:
        raise ValueError(f"invalid application vector: 0x{stack:08x}, 0x{reset:08x}")
    if not APP_START <= (reset & ~1) < APP_END:
        raise ValueError(f"application reset vector escapes slot: 0x{reset:08x}")
    if any(address not in memory for address in range(SOFTDEVICE_MAGIC_ADDRESS, SOFTDEVICE_MAGIC_ADDRESS + 4)):
        raise ValueError("UF2 does not replace the old SoftDevice signature")
    replacement = bytes(memory[address] for address in range(
        SOFTDEVICE_MAGIC_ADDRESS, SOFTDEVICE_MAGIC_ADDRESS + 4
    ))
    if replacement == SOFTDEVICE_MAGIC:
        raise ValueError("UF2 leaves the old SoftDevice signature intact")

    print(f"uf2={path}")
    print(f"sha256={digest}")
    print(f"blocks={len(seen)} address_min=0x{min(memory):x} address_max=0x{max(memory) + 1:x}")
    print(f"stack=0x{stack:08x} reset=0x{reset:08x} softdevice_signature_replaced=true")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify a final UF2 against the factory-compatible layout")
    parser.add_argument("uf2", type=Path)
    parser.add_argument("--confirm-sha256")
    args = parser.parse_args()
    verify(args.uf2, args.confirm_sha256)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
