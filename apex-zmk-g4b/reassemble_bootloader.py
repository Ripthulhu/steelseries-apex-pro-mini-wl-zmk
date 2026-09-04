#!/usr/bin/env python3
"""Reassemble the stock MBR+SoftDevice prefix from APXD dump records.

Each APXD record contains one CRC-protected 4 KiB page. The tool keeps the first
valid copy of every page in 0x00000000..0x0001C000, reports missing pages, and
optionally writes the complete prefix and UICR capture.

This range does not contain the protected factory bootloader. No dump of that
bootloader was obtained.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import struct
from pathlib import Path

MAGIC = b"APXD"
CHUNK = 4096
# magic + version + seq + addr + length + data + crc32
HEADER = struct.calcsize("<IHHII")
RECORD = HEADER + CHUNK + 4

PREFIX_START = 0x00000000
PREFIX_END = 0x0001C000
UICR = 0x10001000

# CRCs recorded independently by the stage-4 page survey.
SURVEY_CRCS = {
    0x00000000: 0xB1E73AE6, 0x00001000: 0xFF109312, 0x00002000: 0x6F2DAD62,
    0x00003000: 0x6D4721E5, 0x00004000: 0x9ED8CC46, 0x00005000: 0x00695061,
    0x00006000: 0x9543EAFF, 0x00007000: 0x9B839974, 0x00008000: 0xA19FB1BB,
    0x00009000: 0x18A6F17C, 0x0000A000: 0x21BCC5F5, 0x0000B000: 0x934AE9DD,
    0x0000C000: 0x7FD3D94E, 0x0000D000: 0x4DF3AF3F, 0x0000E000: 0x7D8CCEF6,
    0x0000F000: 0x0E36E1F6, 0x00010000: 0x8FF8391A, 0x00011000: 0x6C8BEFEE,
    0x00012000: 0xBB62B7B1, 0x00013000: 0x99654077, 0x00014000: 0xB9A089D3,
    0x00015000: 0x4B861F24, 0x00016000: 0xC72E4112, 0x00017000: 0x71AC01BB,
    0x00018000: 0xBA24A97B, 0x00019000: 0x46BDEA35, 0x0001A000: 0x85161C9F,
    0x0001B000: 0x6C477858,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--out", type=Path,
                        help="write the MBR+SoftDevice prefix here")
    parser.add_argument("--uicr-out", type=Path, help="write the UICR page here")
    args = parser.parse_args()

    data = args.capture.read_bytes()
    print(f"capture {args.capture}  {len(data)} bytes")

    pages: dict[int, bytes] = {}
    seen = bad = 0
    offset = 0
    while True:
        index = data.find(MAGIC, offset)
        if index < 0:
            break
        offset = index + 1
        if index + RECORD > len(data):
            continue
        magic, version, seq, addr, length = struct.unpack_from("<IHHII", data, index)
        if version != 8 or length != CHUNK:
            continue
        body = data[index + HEADER : index + HEADER + CHUNK]
        stored = struct.unpack_from("<I", data, index + HEADER + CHUNK)[0]

        # CRC covers addr, length and data - exactly what the firmware hashed.
        computed = binascii.crc32(data[index + 8 : index + HEADER] + body) & 0xFFFFFFFF
        seen += 1
        if computed != stored:
            bad += 1
            continue
        pages.setdefault(addr, body)

    print(f"chunks seen {seen}, CRC-valid {seen - bad}, corrupt {bad}, "
          f"distinct pages {len(pages)}")

    expected = list(range(PREFIX_START, PREFIX_END, CHUNK))
    missing = [a for a in expected if a not in pages]
    print(f"\nstock prefix 0x{PREFIX_START:08X}-0x{PREFIX_END:08X}: "
          f"{len(expected) - len(missing)}/{len(expected)} pages recovered")
    if missing:
        print("  MISSING: " + ", ".join(f"0x{a:08X}" for a in missing))

    agree = disagree = unchecked = 0
    for addr in expected:
        if addr not in pages:
            continue
        want = SURVEY_CRCS.get(addr)
        got = binascii.crc32(pages[addr]) & 0xFFFFFFFF
        if want is None:
            unchecked += 1
        elif want == got:
            agree += 1
        else:
            disagree += 1
            print(f"  !! 0x{addr:08X}: dump CRC 0x{got:08X} != survey 0x{want:08X}")
    print(f"\ncross-check against the stage-4 survey: {agree} agree, "
          f"{disagree} disagree, {unchecked} unchecked")

    if UICR in pages:
        page = pages[UICR]
        print(f"\nUICR page recovered, first 32 bytes: {page[:32].hex(' ')}")
        approtect = struct.unpack_from("<I", page, 0x208)[0]
        print(f"  APPROTECT (+0x208) = 0x{approtect:08X}")
        if args.uicr_out:
            args.uicr_out.write_bytes(page)
            print(f"  written to {args.uicr_out}")
    else:
        print("\nUICR page NOT recovered")

    if not missing and args.out:
        image = b"".join(pages[a] for a in expected)
        args.out.write_bytes(image)
        digest = hashlib.sha256(image).hexdigest()
        print(f"\nstock prefix written to {args.out}")
        print(f"  size   {len(image)} bytes")
        print(f"  sha256 {digest}")
        print(f"  crc32  0x{binascii.crc32(image) & 0xFFFFFFFF:08X}")
    elif missing:
        print("\nNOT writing an image because one or more prefix pages are missing")

    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main())
