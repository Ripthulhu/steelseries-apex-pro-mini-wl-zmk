#!/usr/bin/env python3
"""Generate a deduplicated scanner table from the retained stock capture."""
import argparse
from pathlib import Path

from build_scanner_config import FRAME, parse_capture


def compact(frames):
    if len(frames) != 59 or [n for n, _, _ in frames] != list(range(1, 60)):
        raise ValueError("Expected the 59 ordered scanner exchanges")
    blocks = []
    indices = {}
    exchanges = []
    for _, tx, rx in frames:
        pair = []
        for data in (tx, rx):
            if len(data) != FRAME:
                raise ValueError("Scanner frames must contain 64 bytes")
            block = bytes(data)
            if block not in indices:
                indices[block] = len(blocks)
                blocks.append(block)
            pair.append(indices[block])
        exchanges.append(pair)
    return blocks, exchanges


def render(frames):
    blocks, exchanges = compact(frames)
    lines = [
        "/* Generated from apex_boot_prefix.h. Do not edit. */",
        "#pragma once", "#include <stdint.h>",
        "#define APEX_BOOT_PREFIX_FRAMES 59",
        "#define APEX_SPIM_FRAME_BYTES 64",
        "struct apex_boot_frame { const uint8_t *tx; const uint8_t *expect_rx; };",
        f"static const uint8_t apex_boot_blocks[{len(blocks)}][64] = {{",
    ]
    for block in blocks:
        lines.append("    { " + ", ".join(f"0x{b:02x}" for b in block) + " },")
    lines += ["};", "static const struct apex_boot_frame apex_boot_prefix[59] = {"]
    for tx, rx in exchanges:
        lines.append(f"    {{ apex_boot_blocks[{tx}], apex_boot_blocks[{rx}] }},")
    lines += ["};", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    frames = parse_capture()
    text = render(frames)
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
