#!/usr/bin/env python3
"""Build the standalone ZMK module on Linux, macOS, or Windows."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
MODULE_ROOT = HERE.parent


def fail(message: str) -> None:
    raise RuntimeError(message)


def executable(directory: Path, name: str) -> Path:
    for candidate in (directory / name, directory / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    fail(f"required tool is missing: {directory / name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zmk_root", type=Path)
    parser.add_argument("--build-directory", type=Path,
                        default=Path("build/apex-pro-mini-wl"))
    args = parser.parse_args()
    zmk = args.zmk_root.expanduser().resolve()
    bin_dir = zmk / ".venv" / ("Scripts" if os.name == "nt" else "bin")
    west = executable(bin_dir, "west")
    sdk = zmk / ".zephyr-sdk"
    if not sdk.is_dir():
        fail(f"Zephyr SDK is missing: {sdk}")
    env = os.environ.copy()
    env["ZEPHYR_SDK_INSTALL_DIR"] = sdk.as_posix()
    env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
    command = [str(west), "build", "--pristine", "-d", str(args.build_directory),
               "-b", "apex_pro_mini_wl//zmk", "--",
               f"-DZMK_EXTRA_MODULES={MODULE_ROOT.as_posix()}"]
    print("+", " ".join(command))
    subprocess.run(command, cwd=zmk / "app", env=env, check=True)
    print("Build complete. This script does not flash hardware.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"slot_build=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
