#!/usr/bin/env python3
"""Create the pinned ZMK and bootloader build workspace on Linux, macOS, or Windows."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))


def fail(message: str) -> None:
    raise RuntimeError(message)


def run(command: list[str | Path], *, cwd: Path | None = None,
        env: dict[str, str] | None = None) -> None:
    values = [str(item) for item in command]
    print("+", " ".join(values))
    subprocess.run(values, cwd=cwd, env=env, check=True)


def output(command: list[str | Path], *, cwd: Path | None = None) -> str:
    return subprocess.check_output([str(item) for item in command], cwd=cwd, text=True).strip()


def executable(directory: Path, name: str) -> Path:
    for candidate in (directory / name, directory / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    fail(f"required tool is missing: {directory / name}")


def venv_bin(venv: Path) -> Path:
    return venv / ("Scripts" if os.name == "nt" else "bin")


def locked_patches(directory: Path) -> list[Path]:
    expected = {
        ROOT / relative for relative in LOCK["patches"]
        if (ROOT / relative).parent == directory
    }
    actual = set(directory.glob("*.patch"))
    if actual != expected:
        unexpected = sorted(path.name for path in actual - expected)
        missing = sorted(path.name for path in expected - actual)
        details = []
        if unexpected:
            details.append("unexpected: " + ", ".join(unexpected))
        if missing:
            details.append("missing: " + ", ".join(missing))
        fail(f"patch directory does not match dependencies.lock.json ({'; '.join(details)})")
    return sorted(expected)


def clone_locked(name: str, work_root: Path) -> Path:
    entry = LOCK["repositories"][name]
    path = work_root / entry["path"]
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", entry["url"], path])
    if not (path / ".git").exists():
        fail(f"expected a Git checkout at {path}")
    head = output(["git", "rev-parse", "HEAD"], cwd=path)
    if head != entry["revision"]:
        if output(["git", "status", "--porcelain"], cwd=path):
            fail(f"refusing to change revision in a dirty checkout: {path}")
        run(["git", "fetch", "origin", entry["revision"]], cwd=path)
        run(["git", "checkout", "--detach", entry["revision"]], cwd=path)
    return path


def west_projects_ready(zmk: Path) -> bool:
    modules_file = ROOT / LOCK["zmk_module_list"]
    module_paths = [
        line.strip()
        for line in modules_file.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    return all((zmk / path / ".git").exists() for path in ["zephyr", *module_paths])


def pin_west_repositories(work_root: Path) -> None:
    pinned = {
        entry["path"]: entry["revision"]
        for name, entry in LOCK["repositories"].items()
        if name not in {"zmk", "adafruit_nrf52_bootloader"}
    }
    zmk_path = Path(LOCK["repositories"]["zmk"]["path"])
    pinned.update({
        str(zmk_path / path): revision
        for path, revision in LOCK["west_revisions"].items()
    })
    for relative, revision in pinned.items():
        path = work_root / relative
        if not (path / ".git").exists():
            fail(f"west repository is missing: {path}")
        if output(["git", "rev-parse", "HEAD"], cwd=path) == revision:
            continue
        if output(["git", "status", "--porcelain"], cwd=path):
            fail(f"refusing to change revision in a dirty checkout: {path}")
        present = subprocess.run(
            ["git", "cat-file", "-e", f"{revision}^{{commit}}"], cwd=path,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if present.returncode != 0:
            remotes = output(["git", "remote"], cwd=path).splitlines()
            if not remotes:
                fail(f"cannot fetch the locked revision; no remote is configured: {path}")
            remote = "origin" if "origin" in remotes else sorted(remotes)[0]
            run(["git", "fetch", remote, revision], cwd=path)
        run(["git", "checkout", "--detach", revision], cwd=path)


def apply_patch(checkout: Path, patch: Path) -> None:
    forward = subprocess.run(
        ["git", "apply", "--check", str(patch)], cwd=checkout,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if forward.returncode == 0:
        run(["git", "apply", patch], cwd=checkout)
        return
    reverse = subprocess.run(
        ["git", "apply", "--reverse", "--check", str(patch)], cwd=checkout,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if reverse.returncode != 0:
        fail(f"patch is neither applicable nor already applied: {patch}")
    print(f"already applied: {patch.relative_to(ROOT)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--skip-sdk", action="store_true",
                        help="prepare sources and Python tools without downloading the Zephyr SDK")
    args = parser.parse_args()
    work_root = args.work_root or (
        Path(os.environ["APEX_ZMK_WORK_ROOT"])
        if os.environ.get("APEX_ZMK_WORK_ROOT") else ROOT.parent / "work"
    )
    work_root = work_root.expanduser().resolve()
    work_root.mkdir(parents=True, exist_ok=True)

    run([sys.executable, ROOT / "tools" / "verify_release.py", "--dependencies-only"])
    zmk = clone_locked("zmk", work_root)
    venv = zmk / ".venv"
    if not venv.exists():
        run([sys.executable, "-m", "venv", venv])
    bin_dir = venv_bin(venv)
    python = executable(bin_dir, "python")
    tools = LOCK["host_tools"]
    run([python, "-m", "pip", "install", f"pip=={tools['pip']}"])
    run([python, "-m", "pip", "install", f"west=={tools['west']}",
         f"cmake=={tools['cmake']}", f"ninja=={tools['ninja']}",
         f"protoc-wheel-0=={tools['protoc-wheel-0']}",
         f"protobuf=={tools['protobuf']}"])
    west = executable(bin_dir, "west")
    tool_env = os.environ.copy()
    tool_env["PATH"] = str(bin_dir) + os.pathsep + tool_env.get("PATH", "")
    if not (zmk / ".west" / "config").is_file():
        run([west, "init", "-l", "app"], cwd=zmk, env=tool_env)
    if west_projects_ready(zmk):
        print("west projects are already present")
    else:
        run([west, "update"], cwd=zmk, env=tool_env)
    pin_west_repositories(work_root)
    run([west, "zephyr-export"], cwd=zmk, env=tool_env)
    run([python, "-m", "pip", "install", "-r",
         zmk / "app" / "scripts" / "requirements.txt", "-r",
         zmk / "zephyr" / "scripts" / "requirements.txt"])

    for patch in locked_patches(ROOT / "apex-zmk-g4b" / "patches"):
        apply_patch(zmk, patch)

    sdk = zmk / ".zephyr-sdk"
    sdk_version = sdk / "sdk_version"
    expected_sdk = LOCK["zephyr_sdk"]
    if not args.skip_sdk and (
        not sdk_version.is_file()
        or sdk_version.read_text(encoding="ascii").strip() != expected_sdk
    ):
        if sdk.exists():
            fail(f"remove or move the mismatched SDK before continuing: {sdk}")
        run([west, "sdk", "install", "--version", expected_sdk,
             "--install-dir", sdk, "--toolchains", "arm-zephyr-eabi"],
            cwd=zmk, env=tool_env)

    bootloader = clone_locked("adafruit_nrf52_bootloader", work_root)
    run(["git", "submodule", "update", "--init", "lib/nrfx", "lib/tinycrypt",
         "lib/tinyusb", "lib/uf2"], cwd=bootloader)
    if not args.skip_sdk:
        run([python, ROOT / "tools" / "verify_release.py", "--work-root", work_root,
             "--dependencies-only"])
    print(f"Workspace ready: {work_root}")
    print("Build with: python tools/build_release.py")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"workspace_setup=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
