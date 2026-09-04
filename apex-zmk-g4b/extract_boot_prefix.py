#!/usr/bin/env python3
"""Freeze the stock STM32 boot-configuration frame prefix into a C header.

The frames come from the decoded stock SPIM3 trace
`work/stock-spim-uart-trace/artifacts/com5-correct-context-spim-trace-auto.decoded.txt`,
which contains 235 version-2 records with full 64-byte TX and RX frames.

The first frames of every stock boot are a fixed configuration sequence the
Nordic sends to the STM32 before key scanning starts. G4B-2 replays them
byte-for-byte and compares the full 64-byte response against the recorded one.

Frozen here, with a checksum, so the replay cannot silently drift from the
capture it was derived from. Run with --check in CI/verification to confirm the
committed header still matches the trace.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path


FRAME_RE = re.compile(r"^frame=(\d+) .*? tx_len=(\d+) rx_len=(\d+) .*? crc_ok=(\w+)")
HEX_RE = re.compile(r"^\s+(tx|rx)=([0-9a-f ]+)$")
REPO_ROOT = Path(__file__).resolve().parents[1]
WORK_ROOT = Path(os.environ.get("APEX_ZMK_WORK_ROOT", REPO_ROOT.parent / "work"))
DEFAULT_DECODED = (
    WORK_ROOT
    / "stock-spim-uart-trace"
    / "artifacts"
    / "com5-correct-context-spim-trace-auto.decoded.txt"
)


def parse(decoded: Path) -> list[dict]:
    frames: list[dict] = []
    current: dict | None = None
    for line in decoded.read_text(encoding="ascii", errors="replace").splitlines():
        header = FRAME_RE.match(line)
        if header:
            current = {
                "index": int(header.group(1)),
                "tx_len": int(header.group(2)),
                "rx_len": int(header.group(3)),
                "crc_ok": header.group(4) == "True",
            }
            frames.append(current)
            continue
        payload = HEX_RE.match(line)
        if payload and current is not None:
            current[payload.group(1)] = bytes.fromhex(payload.group(2))
    return frames


def emit(frames: list[dict], count: int) -> str:
    body = []
    for frame in frames[:count]:
        tx = ", ".join(f"0x{b:02x}" for b in frame["tx"])
        rx = ", ".join(f"0x{b:02x}" for b in frame["rx"])
        body.append(f"    /* frame {frame['index']:3d} */ {{")
        body.append(f"        .tx = {{ {tx} }},")
        body.append(f"        .expect_rx = {{ {rx} }},")
        body.append("    },")
    return "\n".join(body)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--decoded",
        type=Path,
        default=DEFAULT_DECODED,
    )
    parser.add_argument("--count", type=int, default=59)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true", help="verify only, write nothing")
    args = parser.parse_args()

    frames = parse(args.decoded)
    if len(frames) < args.count:
        print(f"FAIL: only {len(frames)} frames decoded, need {args.count}")
        return 1

    selected = frames[: args.count]
    for frame in selected:
        if not frame["crc_ok"]:
            print(f"FAIL: frame {frame['index']} is not CRC-valid")
            return 1
        if frame["tx_len"] != 64 or frame["rx_len"] != 64:
            print(f"FAIL: frame {frame['index']} is not 64/64")
            return 1
        if len(frame.get("tx", b"")) != 64 or len(frame.get("rx", b"")) != 64:
            print(f"FAIL: frame {frame['index']} has a short payload")
            return 1

    blob = b"".join(f["tx"] + f["rx"] for f in selected)
    digest = hashlib.sha256(blob).hexdigest()

    # Landmarks the plan relies on, asserted here so a bad extraction is loud.
    landmarks = {
        0: (0x90, "version query"),
        29: (0x20, "scanner state"),
        57: (0x20, "scanner state"),
        58: (0xA1, "first key poll"),
    }
    for idx, (opcode, label) in landmarks.items():
        if idx >= len(selected):
            continue
        actual = selected[idx]["tx"][0]
        status = "ok" if actual == opcode else "MISMATCH"
        print(f"landmark idx={idx:2d} ({label}): tx[0]=0x{actual:02x} expected 0x{opcode:02x} {status}")
        if actual != opcode:
            print("FAIL: landmark mismatch - the trace or the index base has changed")
            return 1

    print(f"frames={len(selected)} sha256={digest}")

    if args.check or not args.output:
        return 0

    header = f"""/* SPDX-License-Identifier: MIT
 *
 * GENERATED - do not edit by hand.
 * Regenerate with apex-zmk-g4b/extract_boot_prefix.py
 *
 * Stock STM32 boot-configuration prefix, {len(selected)} frames of 64 bytes each,
 * captured from stock firmware 3.24.1 over SPIM3.
 *
 * source : {args.decoded.name}
 * sha256 : {digest}
 *
 * Frame 0 is the 0x90 version query whose known response is ASCII "3.24.1".
 * Frames 29 and 57 report scanner state; frame 58 is
 * the first 0xA1 key poll.
 */

#pragma once

#include <stdint.h>

#define APEX_BOOT_PREFIX_FRAMES {len(selected)}
#define APEX_SPIM_FRAME_BYTES   64
#define APEX_BOOT_PREFIX_SHA256 "{digest}"

struct apex_boot_frame {{
    uint8_t tx[APEX_SPIM_FRAME_BYTES];
    uint8_t expect_rx[APEX_SPIM_FRAME_BYTES];
}};

static const struct apex_boot_frame apex_boot_prefix[APEX_BOOT_PREFIX_FRAMES] = {{
{emit(selected, args.count)}
}};
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="ascii")
    print(f"wrote {args.output} ({len(header)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
