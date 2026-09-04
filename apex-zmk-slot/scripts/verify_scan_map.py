#!/usr/bin/env python3
"""Cross-check scan-map.json against the compiled driver's C lookup table."""

from __future__ import annotations

import json
import re
from pathlib import Path


UNUSED = 0xFF
KEY_COUNT = 70


def main() -> int:
    module_root = Path(__file__).resolve().parent.parent
    document = json.loads((module_root / "scan-map.json").read_text(encoding="utf-8"))
    rows = int(document["matrix"]["rows"])
    columns = int(document["matrix"]["columns"])

    expected = [UNUSED] * KEY_COUNT
    matrix_positions: set[int] = set()
    for key in document["keys"]:
        scan_bit = int(key["scan_bit"])
        matrix_position = int(key["row"]) * columns + int(key["column"])
        if not 0 <= scan_bit < KEY_COUNT:
            raise ValueError(f"scan bit out of range: {scan_bit}")
        if expected[scan_bit] != UNUSED:
            raise ValueError(f"duplicate scan bit: {scan_bit}")
        if not 0 <= matrix_position < rows * columns:
            raise ValueError(f"matrix position out of range: {matrix_position}")
        if matrix_position in matrix_positions:
            raise ValueError(f"duplicate matrix position: {matrix_position}")
        expected[scan_bit] = matrix_position
        matrix_positions.add(matrix_position)

    unused = sorted(int(value) for value in document["unused_scan_bits"])
    actual_unused = [index for index, value in enumerate(expected) if value == UNUSED]
    if unused != actual_unused:
        raise ValueError(f"unused scan-bit mismatch: JSON={unused}, derived={actual_unused}")

    source = (module_root / "src" / "kscan_apex_stm32.c").read_text(encoding="utf-8")
    match = re.search(
        r"apex_scan_to_matrix\[APEX_STM32_KEY_COUNT\]\s*=\s*\{(.*?)\};",
        source,
        flags=re.DOTALL,
    )
    if not match:
        raise ValueError("driver lookup table was not found")
    initializer = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
    tokens = [token.strip() for token in initializer.split(",") if token.strip()]
    driver = [UNUSED if token == "APEX_MATRIX_UNUSED" else int(token, 0) for token in tokens]

    if len(driver) != KEY_COUNT:
        raise ValueError(f"driver table has {len(driver)} entries, expected {KEY_COUNT}")
    if driver != expected:
        differences = [
            (index, driver[index], expected[index])
            for index in range(KEY_COUNT)
            if driver[index] != expected[index]
        ]
        raise ValueError(f"driver/JSON mismatch: {differences}")

    print(f"PASS: {len(document['keys'])} keys and {len(unused)} unused scan bits")
    print("PASS: scan-map.json exactly matches apex_scan_to_matrix[]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
