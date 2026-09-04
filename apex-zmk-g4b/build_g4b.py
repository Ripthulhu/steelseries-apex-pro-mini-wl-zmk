#!/usr/bin/env python3
"""Build, package, and verify an Apex G4B application without flashing it."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parent
LOCK = json.loads((REPO / "dependencies.lock.json").read_text(encoding="utf-8"))


def fail(message: str) -> None:
    raise RuntimeError(message)


def executable(directory: Path, name: str) -> Path:
    candidates = [directory / name, directory / f"{name}.exe"]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    fail(f"required tool is missing: {candidates[0]}")


def venv_bin(venv: Path) -> Path:
    candidate = venv / ("Scripts" if os.name == "nt" else "bin")
    if not candidate.is_dir():
        fail(f"Python virtual environment belongs to another operating system: {venv}")
    return candidate


def cmake_path(path: Path) -> str:
    # Keep virtual-environment symlinks intact so CMake uses that interpreter.
    return Path(os.path.abspath(path)).as_posix()


def remove_directory(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    if path.exists():
        fail(f"could not clear output directory: {path}")


def run(command: list[str | Path], *, cwd: Path | None = None,
        env: dict[str, str] | None = None, stdout: Path | None = None) -> None:
    printable = [str(item) for item in command]
    print("+", " ".join(printable))
    if stdout is None:
        subprocess.run(printable, cwd=cwd, env=env, check=True)
    else:
        with stdout.open("w", encoding="ascii", newline="\n") as output:
            subprocess.run(printable, cwd=cwd, env=env, stdout=output, check=True)


def stage_config(args: argparse.Namespace) -> str:
    if args.stage == 0:
        return "g4b0_usbnb.conf" if args.usb_studio and args.no_ble else (
            "g4b0_usb.conf" if args.usb_studio else "g4b0.conf"
        )
    if args.stage == 1:
        return "g4b.conf"
    if args.stage == 2:
        return "g4b2.conf"
    if args.stage == 3:
        if args.usb_studio:
            return "g4b_usb.conf"
        if args.kscan_ingest and args.persistent:
            return "g4c2.conf"
        return "g4c.conf" if args.kscan_ingest else "g4b3.conf"
    if args.stage == 4:
        return "g4b4.conf"
    if args.stage == 5:
        return "g4b5l.conf" if args.dump_loader else "g4b5.conf"
    if args.stage == 6:
        return "g4b6.conf"
    return "g4b8.conf" if args.studio else "g4b7.conf"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=int, choices=range(8), default=1)
    parser.add_argument("--work-root", type=Path)
    parser.add_argument("--beacon-state", action="store_true")
    parser.add_argument("--kscan-ingest", action="store_true")
    parser.add_argument("--studio", action="store_true")
    parser.add_argument("--no-ble", action="store_true")
    parser.add_argument("--dump-loader", action="store_true")
    parser.add_argument("--usb-studio", action="store_true")
    parser.add_argument("--high-wrapper", action="store_true")
    parser.add_argument("--persistent", action="store_true")
    parser.add_argument("--plain-image", action="store_true")
    parser.add_argument("--stop1-canary", action="store_true")
    parser.add_argument("--wireless-idle", "--mode3-canary", action="store_true")
    parser.add_argument("--ab-rollback", "--ab-canary", action="store_true")
    parser.add_argument("--ab-crash-test", action="store_true")
    parser.add_argument(
        "--extra-conf",
        action="append",
        type=Path,
        default=[],
        metavar="FILE",
        help="append a Kconfig fragment after the selected build configuration",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    work_root = args.work_root or (
        Path(os.environ["APEX_ZMK_WORK_ROOT"])
        if os.environ.get("APEX_ZMK_WORK_ROOT") else REPO.parent / "work"
    )
    work_root = work_root.expanduser().resolve()
    upstream = work_root / "zmk-upstream"
    if not upstream.is_dir():
        fail(f"ZMK workspace not found at {upstream}; run tools/setup_workspace.py")

    if args.stop1_canary and args.wireless_idle:
        fail("--stop1-canary and --wireless-idle are mutually exclusive")
    for enabled, name in (
        (args.stop1_canary, "--stop1-canary"),
        (args.wireless_idle, "--wireless-idle"),
        (args.ab_rollback, "--ab-rollback"),
    ):
        if enabled and not (args.stage == 3 and args.usb_studio):
            fail(f"{name} is limited to the stage-3 USB/Bluetooth build")
    if args.ab_crash_test and not args.ab_rollback:
        fail("--ab-crash-test requires --ab-rollback")
    if args.stop1_canary or args.wireless_idle or args.ab_rollback:
        args.plain_image = True
    if args.usb_studio:
        args.high_wrapper = True

    flavor = "-stop1-canary" if args.stop1_canary else (
        "-wireless-idle" if args.wireless_idle else ""
    )
    if args.ab_rollback:
        flavor += "-ab-v2"
    if args.ab_crash_test:
        flavor = "-ab-crash"

    in_work_tree = HERE == work_root / "apex-zmk-g4b"
    build_dir = (HERE / f"build{flavor}") if in_work_tree else (
        work_root / f"build-repo-apex-zmk-g4b{flavor}"
    )
    artifact_dir = (HERE / f"artifacts{flavor}") if in_work_tree else (
        work_root / f"artifacts-repo-apex-zmk-g4b{flavor}"
    )
    slot = REPO / "apex-zmk-slot"
    venv = upstream / ".venv"
    bin_dir = venv_bin(venv)
    python = executable(bin_dir, "python")
    cmake = executable(bin_dir, "cmake")
    ninja = executable(bin_dir, "ninja")
    protoc = executable(bin_dir, "protoc")
    sdk = upstream / ".zephyr-sdk"

    modules_file = HERE / "modules.txt"
    modules = []
    for line in modules_file.read_text(encoding="utf-8").splitlines():
        item = line.strip()
        if item and not item.startswith("#"):
            path = (upstream / item).resolve()
            if not path.is_dir():
                fail(f"ZMK module is missing: {path}")
            modules.append(path)
    modules.extend((slot, HERE))

    conf_name = stage_config(args)
    conf_files = [HERE / conf_name]
    additions = (
        (args.stop1_canary, "g4b_stop1_canary.conf"),
        (args.wireless_idle, "g4b_wireless_idle.conf"),
        (args.ab_rollback, "g4b_ab_v2.conf"),
        (args.ab_crash_test, "g4b_ab_crashtest.conf"),
    )
    conf_files.extend(HERE / name for enabled, name in additions if enabled)
    conf_files.extend(path.expanduser().resolve() for path in args.extra_conf)
    for path in conf_files:
        if not path.is_file():
            fail(f"configuration file is missing: {path}")

    overlay = "g4b_usb.overlay" if args.usb_studio else (
        "g4b_high.overlay" if args.high_wrapper else (
            "g4b6.overlay" if args.stage == 6 else (
                "g4b7.overlay" if args.stage == 7 or args.persistent else "g4b.overlay"
            )
        )
    )
    flash_dev = bool(re.search(
        r"^\s*CONFIG_APEX_G4B_SPINOR_FLASHDEV\s*=\s*y",
        (HERE / conf_name).read_text(encoding="utf-8"), re.MULTILINE,
    ))
    print(f"overlay: {overlay}")
    print(f"stage: {args.stage}  conf: {conf_name}")
    if args.wireless_idle:
        print("wireless scanner idle policy: ON")
    if args.ab_rollback:
        print("A/B rollback v2: ON")
    if args.ab_crash_test:
        print("A/B rollback crash test: ON")
    for path in args.extra_conf:
        print(f"extra configuration: {path.expanduser().resolve()}")
    if flash_dev:
        print("external SPI-NOR NVS: ON")

    env = os.environ.copy()
    env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
    env["PYTHONPATH"] = cmake_path(HERE / "build_shims")
    env["SOURCE_DATE_EPOCH"] = str(LOCK["source_date_epoch"])
    env["ZEPHYR_BASE"] = cmake_path(upstream / "zephyr")
    env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
    env["ZEPHYR_SDK_INSTALL_DIR"] = cmake_path(sdk)
    remove_directory(build_dir)
    dts_flags = []
    if args.usb_studio:
        dts_flags.append("-DZMK_BEHAVIORS_KEEP_ALL")
    if flash_dev:
        dts_flags.append("-DAPEX_G4B_FLASHDEV_DTS")
    run([
        cmake, "-GNinja", "-S", cmake_path(upstream / "app"),
        "-B", cmake_path(build_dir),
        f"-DCMAKE_MAKE_PROGRAM={cmake_path(ninja)}",
        f"-DPython3_EXECUTABLE={cmake_path(python)}",
        f"-DPROTOBUF_PROTOC_EXECUTABLE={cmake_path(protoc)}",
        "-DEXTRA_CPPFLAGS=" + ";".join((
            f"-fdebug-prefix-map={cmake_path(work_root)}=APEX_BUILD",
            f"-fdebug-prefix-map={cmake_path(REPO)}=APEX_SOURCE",
            f"-fmacro-prefix-map={cmake_path(upstream / 'modules')}=ZEPHYR_MODULES",
            f"-fmacro-prefix-map={cmake_path(build_dir)}=BUILD_DIR",
            f"-fmacro-prefix-map={cmake_path(REPO)}=APEX_SOURCE",
        )),
        f"-DZephyr-sdk_DIR={cmake_path(sdk / 'cmake')}",
        "-DZEPHYR_TOOLCHAIN_VARIANT=zephyr",
        "-DZEPHYR_MODULES=" + ";".join(cmake_path(path) for path in modules),
        "-DBOARD=apex_pro_mini_wl/nrf52833/zmk",
        f"-DBOARD_ROOT={cmake_path(slot)}",
        f"-DDTC_OVERLAY_FILE={cmake_path(HERE / overlay)}",
        "-DEXTRA_CONF_FILE=" + ";".join(cmake_path(path) for path in conf_files),
        "-DDTS_EXTRA_CPPFLAGS=" + ";".join(dts_flags),
    ], env=env)
    run([cmake, "--build", build_dir], env=env)

    if args.plain_image:
        expected_parent = HERE if in_work_tree else work_root
        expected_name = f"artifacts{flavor}" if in_work_tree else (
            f"artifacts-repo-apex-zmk-g4b{flavor}"
        )
        if artifact_dir.parent != expected_parent or artifact_dir.name != expected_name:
            fail(f"refusing to clear unexpected artifact directory: {artifact_dir}")
        remove_directory(artifact_dir)
        artifact_dir.mkdir(parents=True)
        zephyr_out = build_dir / "zephyr"
        outputs = {
            "zmk.hex": "apex-zmk-g4b.plain.hex",
            "zmk.bin": "apex-zmk-g4b.plain.bin",
            "zmk.elf": "apex-zmk-g4b.plain.elf",
            ".config": "apex-zmk-g4b.plain.config",
        }
        for source_name, destination_name in outputs.items():
            source = zephyr_out / source_name
            if source.is_file():
                shutil.copy2(source, artifact_dir / destination_name)
        nm = executable(sdk / "arm-zephyr-eabi" / "bin", "arm-zephyr-eabi-nm")
        run([nm, "-n", "-S", zephyr_out / "zmk.elf"],
            stdout=artifact_dir / "apex-zmk-g4b.plain.symbols.txt")
        image = artifact_dir / "apex-zmk-g4b.plain.hex"
        uf2conv = work_root / "tools" / "adafruit-boot" / "lib" / "uf2" / "utils" / "uf2conv.py"
        if not uf2conv.is_file():
            fail(f"UF2 converter is missing: {uf2conv}; run tools/setup_workspace.py")
        run([python, uf2conv, "-c", "-f", "0x621E937A", "-o",
             artifact_dir / "apex-zmk-g4b.plain.uf2", image])
        verify = [
            python, HERE / "verify_g4b_plain.py", f"--build-dir={build_dir}",
            f"--artifact-dir={artifact_dir}", f"--zephyr-base={upstream / 'zephyr'}",
            f"--expect-stage={args.stage}",
        ]
        if args.stop1_canary:
            verify.append("--stop1-canary")
        if args.wireless_idle:
            verify.append("--wireless-idle")
        if args.ab_rollback:
            verify.append("--ab-v2")
        if args.ab_crash_test:
            verify.append("--ab-crash-test")
        run(verify)
        print(f"PlainImage: {image} ({image.stat().st_size} bytes)")
        print("Plain build and verification complete. Nothing was flashed.")
        return 0

    tool_bin = sdk / "arm-zephyr-eabi" / "bin"
    gcc = executable(tool_bin, "arm-zephyr-eabi-gcc")
    objcopy = executable(tool_bin, "arm-zephyr-eabi-objcopy")
    objdump = executable(tool_bin, "arm-zephyr-eabi-objdump")
    readelf = executable(tool_bin, "arm-zephyr-eabi-readelf")
    nm = executable(tool_bin, "arm-zephyr-eabi-nm")
    failsafe = work_root / "apex-zephyr-ble-canary" / "failsafe_handoff.S"
    linker = work_root / "apex-zephyr-usb-canary" / (
        "failsafe_high.ld" if args.high_wrapper else "failsafe.ld"
    )
    wrapper_address = "0x00065000" if args.high_wrapper else "0x0004C000"
    remove_directory(artifact_dir)
    artifact_dir.mkdir(parents=True)
    image_data = (build_dir / "zephyr" / "zmk.bin").read_bytes()
    sp, reset = struct.unpack_from("<II", image_data)
    common = [
        "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-ffreestanding",
        "-fno-builtin", "-fno-stack-protector", "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables", "-ffunction-sections",
        "-fdata-sections", "-Os", "-Wall", "-Wextra", "-Werror",
    ]
    beacon = ["-DUART_BOOT_BEACON", "-DUART_BOOT_BEACON_RESETREAS"]
    if args.beacon_state:
        beacon.append("-DUART_BOOT_BEACON_STATE")
    run([gcc, *common, "-x", "assembler-with-cpp", f"-DCANARY_SP=0x{sp:08x}",
         f"-DCANARY_RESET=0x{reset:08x}", "-DWDT_CRV_TICKS=0x001E0000",
         "-DWDT_RREN_MASK=0x00000080", "-DRETAINED_RECOVERY_COOKIE=0x42",
         *beacon, "-c", failsafe, "-o", artifact_dir / "failsafe.o"])
    run([gcc, *common, "-nostdlib", "-Wl,--gc-sections",
         f"-Wl,-Map,{artifact_dir / 'failsafe.map'}", "-T", linker,
         artifact_dir / "failsafe.o", "-o", artifact_dir / "failsafe.elf"])
    run([objcopy, "-O", "binary", artifact_dir / "failsafe.elf",
         artifact_dir / "failsafe.bin"])
    run([objdump, "-d", artifact_dir / "failsafe.elf"],
        stdout=artifact_dir / "failsafe.disasm.txt")
    run([readelf, "-lW", build_dir / "zephyr" / "zmk.elf"],
        stdout=artifact_dir / "zmk.segments.txt")
    run([nm, "-n", build_dir / "zephyr" / "zmk.elf"],
        stdout=artifact_dir / "zmk.symbols.txt")
    run([python, HERE / "package_g4b.py", build_dir / "zephyr" / "zmk.bin",
         artifact_dir / "failsafe.bin", artifact_dir / "apex-zmk-g4b.vendor.bin",
         f"--expect-sp=0x{sp:08x}", f"--expect-reset=0x{reset:08x}",
         f"--wrapper-address={wrapper_address}"])
    run([python, HERE / "verify_g4b.py", f"--expect-stage={args.stage}",
         f"--build-dir={build_dir}", f"--artifact-dir={artifact_dir}",
         f"--zephyr-base={upstream / 'zephyr'}"])
    print("Build, packaging and verification complete. Nothing was flashed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"build=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
