#!/usr/bin/env python3
"""Build and package the stock-to-UF2 migration image without flashing it."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
LOCK = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))


def fail(message: str) -> None:
    raise RuntimeError(message)


def executable(directory: Path, name: str) -> Path:
    for candidate in (directory / name, directory / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    fail(f"required tool is missing: {directory / name}")


def run(command: list[str | Path], *, stdout: Path | None = None,
        env: dict[str, str] | None = None) -> None:
    values = [str(item) for item in command]
    print("+", " ".join(values))
    if stdout is None:
        subprocess.run(values, check=True, env=env)
    else:
        with stdout.open("w", encoding="ascii", newline="\n") as output:
            subprocess.run(values, stdout=output, check=True, env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bootloader_hex", type=Path)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--output-dir", type=Path, default=HERE / "artifacts")
    args = parser.parse_args()
    work_root = args.work_root or (
        Path(os.environ["APEX_ZMK_WORK_ROOT"])
        if os.environ.get("APEX_ZMK_WORK_ROOT") else ROOT.parent / "work"
    )
    work_root = work_root.expanduser().resolve()
    bootloader_hex = args.bootloader_hex.expanduser().resolve()
    if not bootloader_hex.is_file():
        fail(f"bootloader image is missing: {bootloader_hex}")
    run([sys.executable, ROOT / "tools" / "verify_release.py", "--work-root",
         work_root, "--dependencies-only"])
    zmk = work_root / LOCK["repositories"]["zmk"]["path"]
    tool_bin = zmk / ".zephyr-sdk" / "arm-zephyr-eabi" / "bin"
    gcc = executable(tool_bin, "arm-zephyr-eabi-gcc")
    objcopy = executable(tool_bin, "arm-zephyr-eabi-objcopy")
    objdump = executable(tool_bin, "arm-zephyr-eabi-objdump")
    migration = HERE / "migration"
    artifacts = args.output_dir.expanduser().resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    build_env = os.environ.copy()
    build_env["SOURCE_DATE_EPOCH"] = str(LOCK["source_date_epoch"])
    common = [
        "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-ffreestanding",
        "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections",
        "-fdata-sections", "-Os", "-Wall", "-Wextra", "-Werror",
    ]
    run([gcc, *common, "-c", migration / "startup.S", "-o", artifacts / "startup.o"],
        env=build_env)
    run([gcc, *common, "-std=c11", "-c", migration / "migrate.c",
         "-o", artifacts / "migrate.o"], env=build_env)
    run([gcc, *common, "-nostdlib", "-Wl,--gc-sections",
         f"-Wl,-Map,{artifacts / 'migration.map'}", "-T", migration / "migration.ld",
         artifacts / "startup.o", artifacts / "migrate.o", "-o", artifacts / "migration.elf"],
        env=build_env)
    run([objcopy, "-O", "binary", artifacts / "migration.elf",
         artifacts / "migration.bin"], env=build_env)
    run([objdump, "-d", artifacts / "migration.elf"],
        stdout=artifacts / "migration.disasm.txt", env=build_env)
    output = artifacts / "apex-stock-to-uf2.vendor.bin"
    run([sys.executable, HERE / "package_migration.py", artifacts / "migration.bin",
         bootloader_hex, output])
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    run([sys.executable, HERE / "flash_stock.py", output,
         "--confirm-sha256", digest])
    print("Build and validation complete. Nothing was flashed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"migration_build=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
