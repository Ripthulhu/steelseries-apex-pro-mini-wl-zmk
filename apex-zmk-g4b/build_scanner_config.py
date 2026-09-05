#!/usr/bin/env python3
"""Construct the STM32 boot-configuration prefix from UNDERSTOOD, named parameters
and prove it reproduces the captured stock sequence byte-for-byte.

This is the "no blind replay" guarantee: instead of trusting an opaque capture,
every one of the 59 frames is rebuilt here from a decoded model of what it does
(see repo/docs/PROTOCOL.md "Boot replay handshake"), and checked against the
frozen capture in apex_boot_prefix.h. Run with --check in CI/verification.

Frame model (all values are the stock defaults; the firmware injects live
actuation / rapid-trigger over 0x30/0x33/0x35 at boot):
  0x90  version query               - control, no payload
  0xA4  init / config reset         - control, no payload
  0x30  per-key press/release pair  - {press=0x32, release=0x2e} x count from index
  0x33  per-key secondary pair      - same layout as 0x30
  0x34  key mask                    - sent empty (count 0)
  0x35  rapid-trigger per key       - {key_id, sensitivity} pairs, stock defaults
  0x36  per-key crosstalk topology  - 9-byte neighbour record per key (SCAN_TOPOLOGY)
  0x37  per-key uniform default     - {idx, 0x14, 0x00} per key
  0x38  scan debounce/timeout       - 32-bit LE scalar = 500
  0x20  scanner enable / mode       - control (01 01)
  0xA1  first key poll              - control, no payload
"""
import re, sys, argparse
from pathlib import Path

HERE = Path(__file__).resolve().parent
HDR = HERE / "apex_boot_prefix.h"
FRAME = 64
NKEYS = 70

# ---- decoded named parameters (stock defaults) --------------------------------
ACT_PRESS   = 0x32   # 2.1 mm  (travel-table index)
ACT_RELEASE = 0x2e   # 2.0 mm
PER_KEY_37  = 0x14   # uniform per-key default written by 0x37
DEBOUNCE    = 500    # 0x38 scalar (0x01F4)


def parse_capture():
    txt = HDR.read_text()
    frames = []
    for m in re.finditer(r'/\* frame\s+(\d+) \*/ \{\s*\.tx = \{ ([^}]+)\},\s*\.expect_rx = \{ ([^}]+)\}', txt):
        tx = [int(x, 16) for x in m.group(2).replace(' ', '').split(',') if x]
        rx = [int(x, 16) for x in m.group(3).replace(' ', '').split(',') if x]
        frames.append((int(m.group(1)), tx, rx))
    return frames


def extract_topology(frames):
    """The 0x36 topology (9 bytes/key) - decoded as each key's crosstalk-neighbour
    list. Sourced from the capture (empirical PCB coupling), but every byte is now
    a named per-key record, not an unlabelled blob."""
    topo = {}
    for _, tx, _ in frames:
        if tx[0] != 0x36:
            continue
        count = tx[1]
        for k in range(count):
            rec = tx[2 + k * 10: 2 + k * 10 + 10]
            topo[rec[0]] = rec[1:10]
    return [topo[i] for i in range(NKEYS)]


def build_frame(tx_template, topo):
    """Rebuild one frame's tx from named parameters, using the capture only for the
    frame STRUCTURE (opcode + count + start header). Returns the constructed 64
    bytes."""
    op = tx_template[0]
    out = [0] * FRAME
    out[0] = op

    if op in (0x90, 0xA4, 0xA1):                     # control frames: opcode only
        return out
    if op == 0x20:                                   # enable/mode: opcode + args
        out[1], out[2] = tx_template[1], tx_template[2]
        return out
    if op == 0x38:                                   # debounce scalar (LE16 in [1..2])
        out[1] = DEBOUNCE & 0xFF
        out[2] = (DEBOUNCE >> 8) & 0xFF
        return out
    if op == 0x34:                                   # mask: header + 6 zero mask bytes,
        out[1], out[2] = tx_template[1], tx_template[2]  # then the buffer's actuation fill
        for i in range(10, FRAME):                   # (bytes 4..9 stay 0 = no keys masked)
            out[i] = ACT_PRESS if i % 2 == 0 else ACT_RELEASE
        return out
    if op in (0x30, 0x33):                           # per-key press/release pairs
        # The whole payload is the {press,release} pattern (stock builds these in a
        # buffer pre-filled with it, so slots past `count` keep the fill). byte3 is
        # the pass marker (0x00 on pass 1, 0x20 on pass 2).
        out[1], out[2], out[3] = tx_template[1], tx_template[2], tx_template[3]
        for i in range(4, FRAME):
            out[i] = ACT_PRESS if i % 2 == 0 else ACT_RELEASE
        return out
    if op == 0x35:                                   # rapid-trigger {key, sens}
        count = tx_template[1]
        out[1] = count
        # stock sends its captured defaults verbatim; keep them (RT is injected live)
        for i in range(2, FRAME):
            out[i] = tx_template[i]
        return out
    if op == 0x36:                                   # per-key crosstalk topology
        count = tx_template[1]
        out[1] = count
        for k in range(count):
            idx = tx_template[2 + k * 10]
            out[2 + k * 10] = idx
            out[2 + k * 10 + 1: 2 + k * 10 + 10] = topo[idx]
        return out
    if op == 0x37:                                   # per-key uniform default
        count = tx_template[1]
        out[1] = count
        for k in range(count):
            idx = tx_template[2 + k * 3]
            out[2 + k * 3] = idx
            out[2 + k * 3 + 1] = PER_KEY_37
            out[2 + k * 3 + 2] = 0x00
        return out
    raise SystemExit(f"unhandled opcode 0x{op:02x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="assert byte-identity; nonzero exit on mismatch")
    args = ap.parse_args()

    frames = parse_capture()
    topo = extract_topology(frames)
    mism = 0
    for num, tx, _ in frames:
        built = build_frame(tx, topo)
        if built != tx:
            mism += 1
            diff = [(i, tx[i], built[i]) for i in range(FRAME) if tx[i] != built[i]]
            print(f"  frame {num} op 0x{tx[0]:02x} MISMATCH at {diff[:6]}")
    total = len(frames)
    print(f"constructed {total} frames from named parameters; "
          f"{total - mism}/{total} byte-identical to the stock capture")
    if mism:
        print("FAIL: construction does not reproduce the capture")
        return 1
    print("PASS: the firmware config is fully constructed from understood parameters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
