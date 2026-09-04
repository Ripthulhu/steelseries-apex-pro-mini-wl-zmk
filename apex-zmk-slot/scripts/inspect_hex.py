#!/usr/bin/env python3
"""Validate the address range and checksums of an Intel HEX image."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_int(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--allowed-start", type=parse_int, default=0x0001C000)
    parser.add_argument("--allowed-end", type=parse_int, default=0x00060FFF)
    args = parser.parse_args()

    base = 0
    minimum = None
    maximum = None
    data_bytes = 0
    outside: list[tuple[int, int, int]] = []

    for line_number, text in enumerate(args.image.read_text().splitlines(), 1):
        if not text.startswith(":"):
            raise ValueError(f"line {line_number}: missing ':'")

        record = bytes.fromhex(text[1:])
        if len(record) < 5 or len(record) != record[0] + 5:
            raise ValueError(f"line {line_number}: invalid length")
        if sum(record) & 0xFF:
            raise ValueError(f"line {line_number}: checksum mismatch")

        count = record[0]
        offset = int.from_bytes(record[1:3], "big")
        record_type = record[3]

        if record_type == 0x00:
            start = base + offset
            end = start + count - 1
            minimum = start if minimum is None else min(minimum, start)
            maximum = end if maximum is None else max(maximum, end)
            data_bytes += count
            if start < args.allowed_start or end > args.allowed_end:
                outside.append((line_number, start, end))
        elif record_type == 0x02:
            base = int.from_bytes(record[4:6], "big") << 4
        elif record_type == 0x04:
            base = int.from_bytes(record[4:6], "big") << 16
        elif record_type in (0x01, 0x03, 0x05):
            continue
        else:
            raise ValueError(f"line {line_number}: unsupported record type 0x{record_type:02x}")

    if minimum is None or maximum is None:
        raise ValueError("image contains no data records")

    print(f"image: {args.image}")
    print(f"data bytes: {data_bytes}")
    print(f"occupied addresses: 0x{minimum:08X}-0x{maximum:08X}")
    print(f"allowed addresses:  0x{args.allowed_start:08X}-0x{args.allowed_end:08X}")

    if outside:
        print(f"FAIL: {len(outside)} data records lie outside the allowed range")
        for line_number, start, end in outside[:10]:
            print(f"  line {line_number}: 0x{start:08X}-0x{end:08X}")
        return 2

    print("PASS: every data record is inside the allowed range")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
