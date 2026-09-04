#!/usr/bin/env python3
"""Verify every file listed in a release bundle's SHA256SUMS.txt."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


LINE = re.compile(r"^([0-9a-fA-F]{64})  ([^/\\]+)$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", nargs="?", type=Path, default=Path.cwd())
    args = parser.parse_args()
    bundle = args.bundle.expanduser().resolve()
    manifest = bundle / "SHA256SUMS.txt"
    if not manifest.is_file():
        raise RuntimeError(f"checksum file is missing: {manifest}")
    checked = 0
    for number, raw_line in enumerate(manifest.read_text(encoding="ascii").splitlines(), 1):
        match = LINE.fullmatch(raw_line)
        if not match:
            raise RuntimeError(f"invalid checksum line {number}: {raw_line!r}")
        expected, name = match.groups()
        path = bundle / name
        if not path.is_file():
            raise RuntimeError(f"release file is missing: {name}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual.lower() != expected.lower():
            raise RuntimeError(f"checksum does not match: {name}")
        print(f"OK  {name}")
        checked += 1
    if not checked:
        raise RuntimeError("checksum file is empty")
    print(f"Verified {checked} release files.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"bundle_verify=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
