#!/usr/bin/env python3
"""Build the verified A/B firmware, bootloader, and first-install bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))


def fail(message: str) -> None:
    raise RuntimeError(message)


def executable(directory: Path, name: str) -> Path:
    for candidate in (directory / name, directory / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    fail(f"required tool is missing: {directory / name}")


def venv_bin(venv: Path) -> Path:
    candidate = venv / ("Scripts" if os.name == "nt" else "bin")
    if not candidate.is_dir():
        fail(f"Python virtual environment belongs to another operating system: {venv}")
    return candidate


def cmake_path(path: Path) -> str:
    # Keep virtual-environment symlinks intact so CMake uses that interpreter.
    return Path(os.path.abspath(path)).as_posix()


def locked_patches(directory: Path) -> list[Path]:
    expected = {
        ROOT / relative for relative in LOCK["patches"]
        if (ROOT / relative).parent == directory
    }
    actual = set(directory.glob("*.patch"))
    if actual != expected:
        fail(f"patch directory does not match dependencies.lock.json: {directory}")
    return sorted(expected)


def run(command: list[str | Path], *, cwd: Path | None = None,
        env: dict[str, str] | None = None) -> None:
    values = [str(item) for item in command]
    print("+", " ".join(values))
    subprocess.run(values, cwd=cwd, env=env, check=True)


def normalized_hash(path: Path) -> str:
    data = path.read_bytes()
    if b"\0" not in data:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_kconfig(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    unset = re.compile(r"^# CONFIG_([A-Za-z0-9_]+) is not set$")
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line[7:].split("=", 1)
            values[name] = value
            continue
        match = unset.fullmatch(line)
        if match:
            values[match.group(1)] = "n"
    return values


def verify_release_config(artifact_dir: Path) -> None:
    config_path = artifact_dir / "apex-zmk-g4b.plain.config"
    if not config_path.is_file():
        fail(f"application configuration is missing: {config_path}")
    config = read_kconfig(config_path)
    required = {
        "APEX_G4B_STAGE": "3",
        "APEX_G4B_SPIM_KSCAN": "y",
        "APEX_G4B_KSCAN_INGEST": "y",
        "APEX_G4B_UART_EVIDENCE": "y",
        "APEX_G4B_AB_ROLLBACK": "y",
        "APEX_G4B_AB_AUTOSTAGE": "y",
        "APEX_G4B_AB_CRASHTEST": "n",
        "APEX_G4B_COREDUMP": "y",
        "APEX_G4B_DONGLE_RADIO": "n",
        "APEX_G4B_ESB": "n",
        "APEX_G4B_EVIDENCE_USB": "n",
        "APEX_G4B_KBD_CAPTURE": "n",
        "APEX_G4B_KBD_TELEMETRY": "n",
        "APEX_G4B_LITTLEFS": "n",
        "APEX_G4B_SLEEP_MS": "0",
        "APEX_G4B_STM32_STOP1_IDLE_MS": "0",
        "APEX_G4B_STM32_STOP1_ALLOW_USB": "n",
        "APEX_G4B_UART_EMIT": "n",
        "APEX_G4B_WATCHDOG": "y",
        "ASSERT": "n",
        "BOOT_BANNER": "n",
        "CONSOLE": "n",
        "DEBUG": "n",
        "DEBUG_INFO": "n",
        "DEBUG_OPTIMIZATIONS": "n",
        "FAULT_DUMP": "0",
        "FILE_SYSTEM": "n",
        "INIT_STACKS": "n",
        "LOG": "n",
        "PRINTK": "n",
        "RTT_CONSOLE": "n",
        "SHELL": "n",
        "TEST": "n",
        "THREAD_ANALYZER": "n",
        "TRACING": "n",
        "USE_SEGGER_RTT": "n",
        "ZMK_LOGGING_MINIMAL": "y",
    }
    wrong = {
        name: (config.get(name, "n"), expected)
        for name, expected in required.items()
        if config.get(name, "n") != expected
    }
    if wrong:
        details = ", ".join(
            f"CONFIG_{name}={actual} (expected {expected})"
            for name, (actual, expected) in sorted(wrong.items())
        )
        fail(f"release configuration policy failed: {details}")

    # A local override may tune the documented numeric settings or turn an
    # existing feature off. It must not quietly turn a diagnostic experiment
    # into part of a file labelled as a release. Keeping an allow-list of the
    # enabled board features also makes a newly-added experiment fail closed.
    allowed_apex_features = {
        "APEX_G4B_SPIM_KSCAN",
        "APEX_G4B_UART_EVIDENCE",
        "APEX_G4B_KSCAN_INGEST",
        "APEX_G4B_RGB",
        "APEX_G4B_ANALOG_PROBE",
        "APEX_G4B_GAMEPAD",
        "APEX_G4B_BAG_GUARD",
        "APEX_G4B_TWI",
        "APEX_G4B_CHARGE_LIMIT",
        "APEX_G4B_CHARGE_STORAGE",
        "APEX_G4B_AB_ROLLBACK",
        "APEX_G4B_AB_AUTOSTAGE",
        "APEX_G4B_SPINOR_WRITE",
        "APEX_G4B_DFU_TRIGGER",
        "APEX_G4B_SPINOR_FLASHDEV",
        "APEX_G4B_SPINOR_NVS_PROVISION",
        "APEX_G4B_COREDUMP",
        "APEX_G4B_WATCHDOG",
        "APEX_G4B_SPINOR",
    }
    unexpected = sorted(
        name for name, value in config.items()
        if name.startswith("APEX_G4B_") and value == "y"
        and name not in allowed_apex_features
    )
    if unexpected:
        fail(
            "release configuration enables non-release board features: "
            + ", ".join(f"CONFIG_{name}" for name in unexpected)
        )
    print("Release configuration: A/B enabled; debug output and test features disabled")


def copy_release_file(source: Path, destination: Path) -> None:
    """Copy an artifact while giving text files stable LF line endings."""
    data = source.read_bytes()
    if destination.suffix.lower() in {".cfg", ".hex", ".md", ".py", ".svg", ".txt"}:
        data = data.replace(b"\r\n", b"\n")
    destination.write_bytes(data)


def recipe_hash() -> str:
    files = sorted(
        [path for path in (ROOT / "bootloader").rglob("*") if path.is_file()]
        + [ROOT / "dependencies.lock.json", ROOT / "tools" / "build_release.py"],
        key=lambda path: path.relative_to(ROOT).as_posix(),
    )
    recipe = "\n".join(
        f"{path.relative_to(ROOT).as_posix()}={normalized_hash(path)}" for path in files
    )
    return hashlib.sha256(recipe.encode()).hexdigest()[:12]


def build_app(work_root: Path, python: Path, extra_conf: list[Path]) -> Path:
    command: list[str | Path] = [
        python, ROOT / "apex-zmk-g4b" / "build_g4b.py", "--stage", "3",
        "--usb-studio", "--kscan-ingest", "--persistent", "--plain-image",
        "--wireless-idle", "--ab-rollback", "--work-root", work_root,
    ]
    for path in extra_conf:
        command.extend(("--extra-conf", path))
    run(command)
    artifact_dir = work_root / "artifacts-repo-apex-zmk-g4b-wireless-idle-ab-v2"
    if not artifact_dir.is_dir():
        fail(f"application build did not create {artifact_dir}")
    return artifact_dir


def prepare_bootloader(work_root: Path, cmake: Path, ninja: Path,
                       python: Path, sdk: Path, *, legacy_layout: bool = False) -> Path:
    base = work_root / LOCK["repositories"]["adafruit_nrf52_bootloader"]["path"]
    if not (base / ".git").exists():
        fail(f"Adafruit bootloader checkout is missing: {base}; run tools/setup_workspace.py")
    digest = recipe_hash()
    release_deps = work_root / "release-deps"
    source = release_deps / f"adafruit-boot-{digest}"
    build = source / ("_build_apex_legacy" if legacy_layout else "_build_apex_release")
    marker = source / ".apex-release-recipe"
    if source.exists() and not marker.is_file():
        fail(f"incomplete bootloader release checkout: {source}")
    if not source.exists():
        release_deps.mkdir(parents=True, exist_ok=True)
        revision = LOCK["repositories"]["adafruit_nrf52_bootloader"]["revision"]
        run(["git", "-C", base, "worktree", "add", "--detach", source, revision])
        shutil.copytree(ROOT / "bootloader" / "apex_pro_mini_wl",
                        source / "src" / "boards" / "apex_pro_mini_wl")
        for name in ("ab_promote.c", "ab_promote.h"):
            shutil.copy2(ROOT / "bootloader" / "apex_pro_mini_wl" / name,
                         source / "src" / name)
        shutil.copy2(
            ROOT / "bootloader" / "apex_pro_mini_wl" / "nrf52833_apex_legacy.ld",
            source / "linker" / "nrf52833_apex_legacy.ld",
        )
        for patch in locked_patches(ROOT / "bootloader" / "patches"):
            run(["git", "-C", source, "apply", "--ignore-space-change", patch])
        marker.write_text(digest + "\n", encoding="ascii", newline="\n")
    elif marker.read_text(encoding="ascii").strip() != digest:
        fail(f"bootloader recipe marker does not match: {source}")

    dfu_source = (source / "lib" / "sdk11" / "components" / "libraries" /
                  "bootloader_dfu" / "bootloader.c")
    dfu_text = dfu_source.read_text(encoding="utf-8", errors="replace")
    for token in ("nrf_wdt_started(NRF_WDT)",
                  "nrf_wdt_reload_request_set(NRF_WDT, i)"):
        if token not in dfu_text:
            fail(f"bootloader DFU no longer feeds an inherited watchdog: {token}")

    run(["git", "-C", source, "submodule", "update", "--init",
         "lib/nrfx", "lib/tinycrypt", "lib/tinyusb", "lib/uf2"])
    build_env = os.environ.copy()
    build_env["SOURCE_DATE_EPOCH"] = str(LOCK["source_date_epoch"])
    configure: list[str | Path] = [
        cmake, "-S", cmake_path(source), "-B", cmake_path(build), "-GNinja",
        f"-DCMAKE_MAKE_PROGRAM={cmake_path(ninja)}", "-DBOARD=apex_pro_mini_wl",
        "-DSD_VERSION=7.2.0", f"-DZEPHYR_SDK_INSTALL_DIR={cmake_path(sdk)}",
        f"-DPython_EXECUTABLE={cmake_path(python)}",
    ]
    if legacy_layout:
        configure.append("-DAPEX_LEGACY_LAYOUT=ON")
    run(configure, env=build_env)
    run([cmake, "--build", build, "--target", "bootloader"], env=build_env)
    for name in ("bootloader.hex", "bootloader_mbr.hex", "bootloader_mbr.uf2"):
        if not (build / name).is_file():
            fail(f"bootloader build did not produce {name}")
    return build


def archive_bundle(bundle: Path) -> Path:
    archive = bundle.with_suffix(".zip")
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in sorted(bundle.iterdir(), key=lambda item: item.name):
            if not path.is_file():
                continue
            info = zipfile.ZipInfo(f"{bundle.name}/{path.name}", (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            output.writestr(info, path.read_bytes())
    return archive


def clean_release_directory(release_dir: Path) -> None:
    """Remove only files generated by this script and reject unrelated contents."""
    release_dir.mkdir(parents=True, exist_ok=True)
    for profile in ("ab",):
        bundle = release_dir / f"apex-pro-mini-wl-{profile}"
        if bundle.exists():
            shutil.rmtree(bundle)
        for suffix in (".uf2", ".zip"):
            asset = release_dir / f"apex-pro-mini-wl-{profile}{suffix}"
            if asset.exists():
                asset.unlink()
    bootloader_update = release_dir / "apex-pro-mini-wl-bootloader-update.uf2"
    if bootloader_update.exists():
        bootloader_update.unlink()
    manifest = release_dir / "RELEASE-SHA256SUMS.txt"
    if manifest.exists():
        manifest.unlink()
    leftovers = sorted(path.name for path in release_dir.iterdir())
    if leftovers:
        fail(f"release directory contains unrelated files: {leftovers}")


def verify_release_directory(release_dir: Path) -> None:
    expected = {
        "RELEASE-SHA256SUMS.txt",
        "apex-pro-mini-wl-ab",
        "apex-pro-mini-wl-ab.uf2",
        "apex-pro-mini-wl-ab.zip",
        "apex-pro-mini-wl-bootloader-update.uf2",
    }
    actual = {path.name for path in release_dir.iterdir()}
    if actual != expected:
        fail(
            "release directory has unexpected contents "
            f"(extra={sorted(actual - expected)}, missing={sorted(expected - actual)})"
        )


def package(artifact_dir: Path, boot_build: Path,
            work_root: Path, python: Path) -> tuple[Path, list[Path]]:
    bundle = work_root / "release" / "apex-pro-mini-wl-ab"
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    copies = {
        artifact_dir / "apex-zmk-g4b.plain.hex": bundle / "apex-zmk.hex",
        artifact_dir / "apex-zmk-g4b.plain.uf2": bundle / "apex-zmk.uf2",
        artifact_dir / "apex-zmk-g4b.plain.config": bundle / "apex-zmk.config",
        boot_build / "bootloader_mbr.hex": bundle / "bootloader_mbr.hex",
        boot_build / "bootloader_mbr.uf2": bundle / "apex-bootloader-update.uf2",
    }
    for source, destination in copies.items():
        if not source.is_file():
            fail(f"release input is missing: {source}")
        copy_release_file(source, destination)
    upstream = work_root / LOCK["repositories"]["zmk"]["path"]
    license_copies = {
        ROOT / "LICENSE": bundle / "LICENSE.txt",
        ROOT / "THIRD_PARTY_NOTICES.md": bundle / "THIRD_PARTY_NOTICES.txt",
        upstream / "LICENSE": bundle / "LICENSE-ZMK.txt",
        upstream / "zephyr" / "LICENSE": bundle / "LICENSE-ZEPHYR.txt",
        upstream / "modules" / "lib" / "nanopb" / "LICENSE.txt":
            bundle / "LICENSE-NANOPB.txt",
        upstream / "modules" / "fs" / "littlefs" / "LICENSE.md":
            bundle / "LICENSE-LITTLEFS.txt",
        upstream / "modules" / "crypto" / "mbedtls" / "LICENSE":
            bundle / "LICENSE-MBEDTLS.txt",
        upstream / "modules" / "lib" / "zcbor" / "LICENSE":
            bundle / "LICENSE-ZCBOR.txt",
        upstream / "modules" / "lib" / "picolibc" / "COPYING.picolibc":
            bundle / "LICENSE-PICOLIBC.txt",
        upstream / "modules" / "lib" / "picolibc" / "COPYING.NEWLIB":
            bundle / "LICENSE-NEWLIB.txt",
        boot_build.parent / "LICENSE": bundle / "LICENSE-ADAFRUIT-BOOTLOADER.txt",
        boot_build.parent / "lib" / "tinyusb" / "LICENSE":
            bundle / "LICENSE-TINYUSB.txt",
        boot_build.parent / "lib" / "uf2" / "LICENSE.txt":
            bundle / "LICENSE-UF2.txt",
        boot_build.parent / "lib" / "nrfx" / "LICENSE":
            bundle / "LICENSE-NRFX.txt",
        boot_build.parent / "lib" / "tinycrypt" / "LICENSE":
            bundle / "LICENSE-TINYCRYPT.txt",
        boot_build.parent / "lib" / "softdevice" / "s140_nrf52_7.2.0"
        / "s140_nrf52_7.2.0_licence-agreement.txt":
            bundle / "LICENSE-NORDIC-S140-7.2.0.txt",
        boot_build.parent / "lib" / "softdevice" / "mbr" / "hex"
        / "mbr_nrf52_2.4.1_licence-agreement.txt":
            bundle / "LICENSE-NORDIC-MBR-2.4.1.txt",
    }
    for source, destination in license_copies.items():
        if not source.is_file():
            fail(f"release license is missing: {source}")
        copy_release_file(source, destination)
    run([python, ROOT / "tools" / "swd" / "make_uicr.py",
         bundle / "uicr-open.bin"])
    for config in sorted((ROOT / "tools" / "swd").glob("*.cfg")):
        copy_release_file(config, bundle / config.name)
    copy_release_file(ROOT / "docs" / "keymap.svg", bundle / "KEYMAP.svg")
    copy_release_file(ROOT / "tools" / "verify_bundle.py", bundle / "verify_bundle.py")
    (bundle / "README.txt").write_text(
        "First-generation SteelSeries Apex Pro Mini Wireless - ZMK firmware\n\n"
        "For a normal update, copy apex-zmk.uf2 to the APEXBOOT drive.\n"
        "apex-bootloader-update.uf2 updates APEXBOOT itself. Most users do not\n"
        "need it. Use it only after APEXBOOT is already installed, then install\n"
        "apex-zmk.uf2 from the same release.\n"
        "apex-zmk.config records the exact options used for this build.\n"
        "KEYMAP.svg shows the default and Fn-layer bindings.\n"
        "For a first installation or repair, follow the current instructions at:\n"
        "https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk\n\n"
        "The first installation erases the original Nordic firmware. Verify this\n"
        "download before flashing by running: python3 verify_bundle.py\n"
        "(use python instead if that is its name on your computer)\n",
        encoding="utf-8", newline="\n",
    )
    manifest = [
        f"{file_hash(path)}  {path.name}"
        for path in sorted(bundle.iterdir(), key=lambda item: item.name)
        if path.is_file() and path.name != "SHA256SUMS.txt"
    ]
    (bundle / "SHA256SUMS.txt").write_text(
        "\n".join(manifest) + "\n", encoding="ascii", newline="\n"
    )
    run([python, bundle / "verify_bundle.py", bundle])
    update = bundle.parent / f"{bundle.name}.uf2"
    shutil.copy2(bundle / "apex-zmk.uf2", update)
    bootloader_update = bundle.parent / "apex-pro-mini-wl-bootloader-update.uf2"
    shutil.copy2(bundle / "apex-bootloader-update.uf2", bootloader_update)
    archive = archive_bundle(bundle)
    print(f"Release bundle: {bundle}")
    print(f"Ready-to-flash update: {update}")
    print(f"Bootloader update: {bootloader_update}")
    print(f"Complete bundle: {archive}")
    return bundle, [update, bootloader_update, archive]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--skip-bootloader", action="store_true")
    parser.add_argument(
        "--installer-bootloader",
        action="store_true",
        help="build only the factory-layout bootloader for the experimental USB installer",
    )
    parser.add_argument(
        "--extra-conf",
        action="append",
        type=Path,
        default=[],
        metavar="FILE",
        help="append a Kconfig fragment after the release configuration",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    work_root = args.work_root or (
        Path(os.environ["APEX_ZMK_WORK_ROOT"])
        if os.environ.get("APEX_ZMK_WORK_ROOT") else ROOT.parent / "work"
    )
    work_root = work_root.expanduser().resolve()
    upstream = work_root / "zmk-upstream"
    bin_dir = venv_bin(upstream / ".venv")
    python = executable(bin_dir, "python")
    cmake = executable(bin_dir, "cmake")
    ninja = executable(bin_dir, "ninja")
    sdk = upstream / ".zephyr-sdk"
    run([python, ROOT / "tools" / "verify_release.py", "--work-root", work_root,
         "--dependencies-only"])
    if args.installer_bootloader:
        if args.skip_bootloader or args.extra_conf:
            fail("--installer-bootloader cannot be combined with application build options")
        boot_build = prepare_bootloader(
            work_root, cmake, ninja, python, sdk, legacy_layout=True
        )
        output_dir = work_root / "installer"
        output_dir.mkdir(parents=True, exist_ok=True)
        output = output_dir / "bootloader-legacy.hex"
        copy_release_file(boot_build / "bootloader.hex", output)
        print(f"Installer bootloader: {output}")
        print("Factory-layout bootloader built. Nothing was flashed.")
        return 0

    extra_conf = [path.expanduser().resolve() for path in args.extra_conf]
    artifact_dir = build_app(work_root, python, extra_conf)
    verify_release_config(artifact_dir)
    run([python, ROOT / "installer" / "verify_final_uf2.py",
         artifact_dir / "apex-zmk-g4b.plain.uf2"])
    if args.skip_bootloader:
        run([python, ROOT / "tools" / "verify_release.py", "--work-root", work_root])
        print("A/B release firmware built and verified. Nothing was flashed.")
        return 0
    boot_build = prepare_bootloader(work_root, cmake, ninja, python, sdk)
    run([python, ROOT / "tools" / "verify_bootloader_update.py",
         boot_build / "bootloader_mbr.uf2",
         artifact_dir / "apex-zmk-g4b.plain.uf2"])
    run([python, ROOT / "tools" / "verify_release.py", "--work-root", work_root])
    release_dir = work_root / "release"
    clean_release_directory(release_dir)
    _, release_assets = package(artifact_dir, boot_build, work_root, python)
    release_manifest = release_dir / "RELEASE-SHA256SUMS.txt"
    release_manifest.write_text(
        "\n".join(
            f"{file_hash(path)}  {path.name}" for path in sorted(release_assets)
        ) + "\n",
        encoding="ascii",
        newline="\n",
    )
    verify_release_directory(release_dir)
    print(f"Release checksums: {release_manifest}")
    print("A/B release firmware built and verified. Nothing was flashed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"release_build=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
