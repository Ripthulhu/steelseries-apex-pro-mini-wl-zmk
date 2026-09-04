#!/usr/bin/env python3
"""Cross-check the whole scan-bit to keycap chain, from files alone.

A pressed key travels through four independent artefacts before it becomes a
character, and each was written at a different time from different evidence:

    STM32 bitmap bit  --(kscan_g4b.c apex_scan_to_matrix)-->  matrix position
    matrix position   --(row = pos/14, col = pos%14)       ->  (row, col)
    (row, col)        --(.dts zmk,matrix-transform map)     ->  keymap index
    keymap index      --(.keymap default_layer bindings)    ->  keycap

`scan-map.json` independently records, per key, both the scan bit and the
(row, column) it should land on. That makes the chain checkable without
hardware: if the kscan table and the scan map disagree, or a mapped position is
missing from the transform, or the keymap has a different key at that index,
every affected key will type the wrong character and require another hardware
test to find.

Read-only. Prints a report and exits non-zero if anything disagrees.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SLOT = ROOT.parent / "apex-zmk-slot"
BOARD = SLOT / "boards" / "steelseries" / "apex_pro_mini_wl"

SCAN_MAP = SLOT / "scan-map.json"
KSCAN_C = ROOT / "src" / "kscan_g4b.c"
DTS = BOARD / "apex_pro_mini_wl_nrf52833_zmk.dts"
KEYMAP = BOARD / "apex_pro_mini_wl.keymap"

COLUMNS = 14
UNUSED = 255

# scan-map.json and ZMK spell the same keycap differently - "LEFT_BRACKET"
# against "LBKT". Keys are compared with underscores removed, so only genuine
# abbreviations need to appear here. Anything not covered is reported rather
# than assumed equal, which is the point: a silent fallback would let a real
# mismatch pass as a naming difference.
ALIASES = {
    "1": "N1", "2": "N2", "3": "N3", "4": "N4", "5": "N5",
    "6": "N6", "7": "N7", "8": "N8", "9": "N9", "0": "N0",
    "BACKSPACE": "BSPC", "CAPSLOCK": "CLCK", "CAPS": "CLCK",
    "ENTER": "RET", "RETURN": "RET",
    "LEFTSHIFT": "LSHFT", "RIGHTSHIFT": "RSHFT",
    "LEFTCTRL": "LCTRL", "RIGHTCTRL": "RCTRL",
    "LEFTALT": "LALT", "RIGHTALT": "RALT",
    "LEFTGUI": "LGUI", "RIGHTGUI": "RGUI",
    "SLASH": "FSLH", "BACKSLASH": "BSLH",
    "SEMICOLON": "SEMI", "QUOTE": "SQT", "APOSTROPHE": "SQT",
    "PERIOD": "DOT",
    "LEFTBRACKET": "LBKT", "RIGHTBRACKET": "RBKT",
    "LBRACKET": "LBKT", "RBRACKET": "RBKT",
    "FN": None,  # no ZMK keycap; the keymap uses a layer behavior here
}

# Physical legends do not always describe the deliberate base-layer binding.
# Keep the exceptions explicit so accidental remaps are still caught elsewhere.
INTENTIONAL_BINDINGS = {
    "CAPS_LOCK": "&mo 1",
    "FN": "&mo 1",
}

problems: list[str] = []


def note(message: str) -> None:
    problems.append(message)


def normalise(key: str) -> str | None:
    key = key.strip().upper().replace("_", "")
    if key in ALIASES:
        return ALIASES[key]
    return key


def parse_kscan_table() -> list[int]:
    text = KSCAN_C.read_text(encoding="utf-8", errors="replace")
    body = re.search(
        r"apex_scan_to_matrix\[[^\]]*\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if body is None:
        note("could not find apex_scan_to_matrix in kscan_g4b.c")
        return []
    stripped = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
    values = []
    for token in stripped.split(","):
        token = token.strip()
        if not token:
            continue
        if token == "APEX_MATRIX_UNUSED":
            values.append(UNUSED)
        elif token.isdigit():
            values.append(int(token))
        else:
            note(f"unparsed token in apex_scan_to_matrix: {token!r}")
    return values


def parse_transform() -> list[tuple[int, int]]:
    text = DTS.read_text(encoding="utf-8", errors="replace")
    block = re.search(r'compatible = "zmk,matrix-transform".*?map = <(.*?)>;',
                      text, re.S)
    if block is None:
        note("could not find the zmk,matrix-transform map in the .dts")
        return []
    return [(int(r), int(c))
            for r, c in re.findall(r"RC\((\d+),\s*(\d+)\)", block.group(1))]


def parse_keymap() -> list[str]:
    text = KEYMAP.read_text(encoding="utf-8", errors="replace")
    block = re.search(r"default_layer\s*\{.*?bindings = <(.*?)>;", text, re.S)
    if block is None:
        note("could not find default_layer bindings in the keymap")
        return []
    # One entry per behavior invocation. &kp X -> X; anything else keeps its
    # own text so a layer/bt binding is visible rather than silently dropped.
    entries = []
    for match in re.finditer(r"&(\w+)((?:\s+[A-Za-z0-9_]+)*)", block.group(1)):
        behavior, args = match.group(1), match.group(2).split()
        entries.append(args[0] if behavior == "kp" and args else
                       f"&{behavior} {' '.join(args)}".strip())
    return entries


def main() -> int:
    scan = json.loads(SCAN_MAP.read_text(encoding="utf-8"))
    keys = scan["keys"]
    table = parse_kscan_table()
    transform = parse_transform()
    keymap = parse_keymap()

    print(f"scan-map keys      : {len(keys)}")
    print(f"kscan table entries: {len(table)}")
    print(f"transform positions: {len(transform)}")
    print(f"keymap bindings    : {len(keymap)}")

    if len(transform) != len(keymap):
        note(f"transform has {len(transform)} positions but the keymap has "
             f"{len(keymap)} bindings; they must correspond one to one")

    position_to_index = {rc: i for i, rc in enumerate(transform)}
    if len(position_to_index) != len(transform):
        note("the transform maps the same (row, column) twice")

    checked = 0
    for entry in keys:
        name, bit = entry["key"], entry["scan_bit"]
        row, col = entry["row"], entry["column"]
        expected = row * COLUMNS + col

        if bit >= len(table):
            note(f"{name}: scan bit {bit} is past the end of the kscan table")
            continue
        actual = table[bit]
        if actual == UNUSED:
            note(f"{name}: scan bit {bit} is UNUSED in the kscan table but "
                 f"scan-map places it at row {row} column {col}")
            continue
        if actual != expected:
            note(f"{name}: kscan maps scan bit {bit} to matrix {actual} "
                 f"(row {actual // COLUMNS} column {actual % COLUMNS}) but "
                 f"scan-map says row {row} column {col} (matrix {expected})")
            continue

        if (row, col) not in position_to_index:
            note(f"{name}: (row {row}, column {col}) is absent from the "
                 f"transform, so the key can never reach the keymap")
            continue

        index = position_to_index[(row, col)]
        if index >= len(keymap):
            note(f"{name}: transform index {index} is past the end of the keymap")
            continue

        got = keymap[index]
        intentional = INTENTIONAL_BINDINGS.get(name)
        if intentional is not None:
            if got != intentional:
                note(f"{name}: keymap index {index} (row {row} column {col}) is "
                     f"{got!r}, expected intentional binding {intentional!r}")
            else:
                print(f"  note: {name} intentionally maps to {got!r}")
        else:
            want = normalise(name)
            if want is None:
                print(f"  note: {name} has no ZMK keycap; keymap has {got!r}")
            elif got != want:
                note(f"{name}: keymap index {index} (row {row} column {col}) is "
                     f"{got!r}, expected {want!r}")
        checked += 1

    # The converse direction: a table entry that maps a bit scan-map.json does
    # not know about is a phantom key. It would emit a keypress from a bit whose
    # meaning was never established by capture, and nothing else here would
    # notice, because every check above starts from the scan map.
    known_bits = {entry["scan_bit"] for entry in keys}
    for bit, matrix in enumerate(table):
        if matrix != UNUSED and bit not in known_bits:
            note(f"kscan maps scan bit {bit} to matrix {matrix} "
                 f"(row {matrix // COLUMNS} column {matrix % COLUMNS}) but "
                 f"scan-map has no key on that bit - phantom mapping")

    # Positions the transform exposes that no scan bit can ever reach.
    reachable = set()
    for entry in keys:
        bit = entry["scan_bit"]
        if bit < len(table) and table[bit] != UNUSED:
            reachable.add((table[bit] // COLUMNS, table[bit] % COLUMNS))
    orphans = [rc for rc in transform if rc not in reachable]
    if orphans:
        print(f"\n  transform positions no scan bit reaches: {len(orphans)}")
        for rc in orphans:
            index = position_to_index[rc]
            binding = keymap[index] if index < len(keymap) else "?"
            print(f"    row {rc[0]} column {rc[1]} -> keymap[{index}] = {binding!r}")

    print(f"\nfully checked: {checked}/{len(keys)} keys")
    if problems:
        print(f"\nDISAGREEMENTS: {len(problems)}")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("OK - kscan table, scan map, transform and keymap all agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
