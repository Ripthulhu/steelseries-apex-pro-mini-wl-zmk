#!/usr/bin/env python3
"""Verify an nRF52833 flash/UICR backup pair against its manifest."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from program_keyboard import BACKUP_MANIFEST, checked_backup_pair


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flash", type=Path)
    parser.add_argument("uicr", type=Path)
    parser.add_argument(
        "--manifest",
        type=Path,
        help=f"pair manifest (default: {BACKUP_MANIFEST} beside the backups)",
    )
    args = parser.parse_args()
    checked_backup_pair(args.flash, args.uicr, args.manifest)
    print("Backup files match their manifest.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"backup_verify=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
