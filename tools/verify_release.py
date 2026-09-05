#!/usr/bin/env python3
"""Run source, provenance, and dependency checks for a release build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "dependencies.lock.json"


def fail(message: str) -> None:
    raise RuntimeError(message)


def run(*command: str, cwd: Path = ROOT) -> None:
    print("+", " ".join(command))
    result = subprocess.run(command, cwd=cwd, check=False)
    if result.returncode:
        fail(f"command failed with exit code {result.returncode}: {' '.join(command)}")


def git_output(path: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(path), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode:
        fail(result.stderr.strip() or f"git failed in {path}")
    return result.stdout.strip()


def load_lock() -> dict:
    data = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    if data.get("format") != 1:
        fail("unsupported dependencies.lock.json format")
    if not isinstance(data.get("source_date_epoch"), int) or data["source_date_epoch"] <= 0:
        fail("dependencies.lock.json needs a positive source_date_epoch")
    return data


def check_patches(lock: dict) -> None:
    locked_paths = {ROOT / relative for relative in lock["patches"]}
    patch_dirs = {path.parent for path in locked_paths}
    found_paths = {path for directory in patch_dirs for path in directory.glob("*.patch")}
    if found_paths != locked_paths:
        extras = sorted(str(path.relative_to(ROOT)) for path in found_paths - locked_paths)
        missing = sorted(str(path.relative_to(ROOT)) for path in locked_paths - found_paths)
        fail(f"patch set differs from lock (extra={extras}, missing={missing})")
    for relative, expected in lock["patches"].items():
        path = ROOT / relative
        if not path.is_file():
            fail(f"locked patch is missing: {relative}")
        # Git can materialize text files with platform-specific line endings.
        # Lock the content, not the checkout convention.
        contents = path.read_bytes().replace(b"\r\n", b"\n")
        actual = hashlib.sha256(contents).hexdigest()
        if actual != expected:
            fail(f"locked patch changed: {relative} ({actual} != {expected})")
    print(f"patches=PASS ({len(lock['patches'])})")


def read_modules(lock: dict) -> list[str]:
    path = ROOT / lock["zmk_module_list"]
    modules = [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not modules or len(modules) != len(set(modules)):
        fail("module list is empty or contains duplicates")
    for module in modules:
        pure = PurePosixPath(module)
        if pure.is_absolute() or ".." in pure.parts or ":" in module or "\\" in module:
            fail(f"module path must be relative and portable: {module}")
    revisions = lock.get("west_revisions", {})
    if set(revisions) != set(modules):
        fail("west_revisions must contain exactly the paths in the ZMK module list")
    print(f"module_list=PASS ({len(modules)})")
    return modules


def check_boot_prefix() -> None:
    path = ROOT / "apex-zmk-g4b" / "apex_boot_prefix.h"
    source = path.read_text(encoding="ascii")
    expected_count = int(re.search(r"APEX_BOOT_PREFIX_FRAMES\s+(\d+)", source).group(1))
    expected_hash = re.search(r'APEX_BOOT_PREFIX_SHA256\s+"([0-9a-f]{64})"', source).group(1)
    bodies = re.findall(
        r"\.tx\s*=\s*\{([^}]*)\}.*?\.expect_rx\s*=\s*\{([^}]*)\}",
        source,
        flags=re.DOTALL,
    )
    if len(bodies) != expected_count:
        fail(f"boot prefix has {len(bodies)} frames, expected {expected_count}")
    blob = bytearray()
    for index, (tx_text, rx_text) in enumerate(bodies):
        tx = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", tx_text))
        rx = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", rx_text))
        if len(tx) != 64 or len(rx) != 64:
            fail(f"boot prefix frame {index} is not 64/64 bytes")
        blob.extend(tx)
        blob.extend(rx)
    actual_hash = hashlib.sha256(blob).hexdigest()
    if actual_hash != expected_hash:
        fail(f"boot prefix checksum changed ({actual_hash} != {expected_hash})")
    print(f"boot_prefix=PASS ({expected_count} frames, {actual_hash})")

    # The config is not a blind capture: prove every frame is reconstructible from
    # the decoded named parameters (build_scanner_config.py). If this fails, either
    # the capture drifted or the decoded model is incomplete - both must be fixed.
    constructor = ROOT / "apex-zmk-g4b" / "build_scanner_config.py"
    result = subprocess.run(
        [sys.executable, str(constructor), "--check"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        fail(f"scanner config no longer constructs from named parameters:\n{result.stdout}{result.stderr}")
    print("boot_prefix_constructed=PASS (59 frames rebuilt from named parameters)")


def check_bootloader_recovery_files() -> None:
    sources = {
        "bootloader/apex_pro_mini_wl/ab_promote.c": (
            "ab_promote_info_append",
            "ab_promote_crash_file",
            '"Bootloader update: supported\\r\\nReset: "',
            "memcpy(newest_crash, record, sizeof(newest_crash))",
        ),
        "bootloader/apex_pro_mini_wl/ab_promote.h": (
            "void ab_promote_info_append(char *text, uint32_t capacity);",
            "uint32_t ab_promote_crash_file(const uint8_t **data);",
        ),
        "bootloader/patches/0008-apex-recovery-info.patch": (
            'name = "CRASH   BIN"',
            "ab_promote_info_append(infoUf2File, sizeof(infoUf2File));",
            "ab_promote_crash_file(&info[2].content)",
        ),
    }
    for relative, required in sources.items():
        text = (ROOT / relative).read_text(encoding="utf-8")
        for token in required:
            if token not in text:
                fail(f"bootloader recovery support is incomplete: {relative} lacks {token!r}")
    print("bootloader_recovery_files=PASS")


def check_workspace(lock: dict, work_root: Path, modules: list[str]) -> None:
    work_root = work_root.resolve()
    for name, entry in lock["repositories"].items():
        path = work_root / entry["path"]
        if not path.is_dir():
            fail(f"locked repository is missing: {name} ({path})")
        actual = git_output(path, "rev-parse", "HEAD")
        if actual != entry["revision"]:
            fail(f"{name} is at {actual}, expected {entry['revision']}")
    zmk = work_root / lock["repositories"]["zmk"]["path"]
    revisions = lock["west_revisions"]
    for module, expected in revisions.items():
        path = zmk / module
        if not path.is_dir():
            fail(f"ZMK module is missing: {module}")
        actual = git_output(path, "rev-parse", "HEAD")
        if actual != expected:
            fail(f"{module} is at {actual}, expected {expected}")
    sdk = zmk / ".zephyr-sdk" / "sdk_version"
    if not sdk.is_file() or sdk.read_text(encoding="ascii").strip() != lock["zephyr_sdk"]:
        fail(f"Zephyr SDK is not the locked {lock['zephyr_sdk']} release")
    bin_dir = zmk / ".venv" / ("Scripts" if os.name == "nt" else "bin")
    python = bin_dir / ("python.exe" if os.name == "nt" else "python")
    if not python.is_file():
        fail(f"prepared Python environment is missing: {python}")
    version_command = [
        str(python), "-c",
        "import importlib.metadata,sys; print(importlib.metadata.version(sys.argv[1]))",
    ]
    for package, expected in lock["host_tools"].items():
        result = subprocess.run(
            [*version_command, package], check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        actual = result.stdout.strip()
        if result.returncode or actual != expected:
            fail(f"{package} is {actual or 'missing'}, expected {expected}")
    print(f"workspace=PASS ({work_root})")


def tracked_python_files() -> list[Path]:
    output = git_output(
        ROOT, "ls-files", "--cached", "--others", "--exclude-standard", "--", "*.py"
    )
    return [ROOT / line for line in output.splitlines() if line and (ROOT / line).is_file()]


def check_python_sources() -> None:
    files = tracked_python_files()
    for path in files:
        try:
            compile(path.read_bytes(), str(path), "exec")
        except SyntaxError as error:
            fail(f"Python syntax error in {path.relative_to(ROOT)}: {error}")
    print(f"python_sources=PASS ({len(files)})")


def run_source_checks() -> None:
    check_python_sources()
    run(sys.executable, str(ROOT / "apex-zmk-slot" / "scripts" / "verify_scan_map.py"))
    run(sys.executable, str(ROOT / "apex-zmk-g4b" / "check_keymap_chain.py"))
    run(sys.executable, str(ROOT / "tools" / "swd" / "make_uicr.py"), "--check")
    run(
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-s",
        str(ROOT / "installer"),
        "-p",
        "test_*.py",
        "-v",
    )
    run(
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-s",
        str(ROOT / "tools" / "swd"),
        "-p",
        "test_*.py",
        "-v",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--dependencies-only", action="store_true")
    args = parser.parse_args()
    try:
        lock = load_lock()
        check_patches(lock)
        modules = read_modules(lock)
        check_boot_prefix()
        check_bootloader_recovery_files()
        if args.work_root:
            check_workspace(lock, args.work_root, modules)
        if not args.dependencies_only:
            run_source_checks()
    except (OSError, ValueError, RuntimeError, AttributeError) as error:
        print(f"release_verification=FAIL: {error}", file=sys.stderr)
        return 1
    print("release_verification=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
