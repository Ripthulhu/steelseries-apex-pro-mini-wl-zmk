#!/usr/bin/env python3
"""Check, install, update, or back up an Apex keyboard through OpenOCD."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import uuid
from pathlib import Path


HERE = Path(__file__).resolve().parent
PROBES = {
    "pi4": "pi4.cfg",
    "pico": "pico-debugprobe.cfg",
    "stlink": "stlink.cfg",
    "jlink": "jlink.cfg",
}
FILES = ("bootloader_mbr.hex", "apex-zmk.hex", "uicr-open.bin")
BACKUP_MANIFEST = "backup-manifest.json"
BACKUP_FORMAT = "apex-swd-backup-v1"
BACKUP_TARGET = "nRF52833"
BACKUP_FILES = {
    "flash": ("flash-open.bin", 0x00000000, 0x80000),
    "uicr": ("uicr-open-backup.bin", 0x10001000, 0x1000),
}
OPEN_BOOTLOADER_UICR_WORDS = {
    0x014: 0x00074000,  # bootloader address
    0x018: 0x0007E000,  # MBR parameter page
    0x200: 0x00000012,  # reset pin P0.18
    0x204: 0x00000012,  # reset pin P0.18
    0x208: 0xFFFFFFFF,  # debug access open
    0x20C: 0xFFFFFFFE,  # NFC pins configured as GPIO
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def find_file(bundle: Path, name: str) -> Path:
    for candidate in (bundle / name, HERE / name):
        if candidate.is_file():
            return candidate.resolve()
    fail(f"required file is missing: {name}")


def find_openocd(value: str) -> str:
    candidate = Path(value).expanduser()
    if candidate.is_file():
        return str(candidate.resolve())
    resolved = shutil.which(value)
    if resolved:
        return resolved
    fail("OpenOCD was not found; install it or pass --openocd /path/to/openocd")


def tcl_path(path: Path) -> str:
    value = path.as_posix()
    if "}" in value:
        fail(f"OpenOCD cannot safely quote this path: {path}")
    return "{" + value + "}"


def verify_bundle(bundle: Path) -> None:
    manifest = bundle / "SHA256SUMS.txt"
    if not manifest.is_file():
        fail(f"checksum file is missing: {manifest}")
    expected = {}
    for line in manifest.read_text(encoding="ascii").splitlines():
        digest, separator, name = line.partition("  ")
        if not separator or len(digest) != 64 or Path(name).name != name:
            fail("SHA256SUMS.txt is invalid")
        expected[name] = digest.lower()
    for name in FILES:
        path = bundle / name
        if not path.is_file():
            fail(f"release file is missing: {name}")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if expected.get(name) != actual:
            fail(f"checksum does not match: {name}")
        print(f"OK  {name}")


def backup_digest(path: Path, expected_size: int) -> str:
    if not path.is_file():
        fail(f"backup file is missing: {path}")
    size = path.stat().st_size
    if size != expected_size:
        fail(f"backup has the wrong size: {path} ({size}, expected {expected_size})")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_backup_manifest(path: Path, flash_digest: str, uicr_digest: str) -> None:
    records = {}
    for role, (name, address, size) in BACKUP_FILES.items():
        records[role] = {
            "name": name,
            "address": f"0x{address:08x}",
            "size": size,
            "sha256": flash_digest if role == "flash" else uicr_digest,
        }
    document = {
        "format": BACKUP_FORMAT,
        "target": BACKUP_TARGET,
        "files": records,
    }
    with path.open("w", encoding="ascii", newline="\n") as output:
        json.dump(document, output, indent=2, sort_keys=True)
        output.write("\n")


def load_backup_manifest(path: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"backup manifest is unreadable: {path}: {error}")
    if not isinstance(document, dict):
        fail(f"backup manifest has an invalid top-level value: {path}")
    if document.get("format") != BACKUP_FORMAT:
        fail(f"backup manifest has an unsupported format: {path}")
    if document.get("target") != BACKUP_TARGET:
        fail(f"backup manifest is for a different target: {path}")
    if not isinstance(document.get("files"), dict):
        fail(f"backup manifest has no valid files table: {path}")
    return document


def checked_backup_pair(
    flash_value: Path, uicr_value: Path, manifest_value: Path | None
) -> tuple[Path, Path]:
    flash = flash_value.expanduser().resolve()
    uicr = uicr_value.expanduser().resolve()
    if manifest_value is None:
        if flash.parent != uicr.parent:
            fail("--manifest is required when the two backup files are in different folders")
        manifest = flash.parent / BACKUP_MANIFEST
    else:
        manifest = manifest_value.expanduser().resolve()
    if not manifest.is_file():
        fail(f"backup manifest is missing: {manifest}")

    document = load_backup_manifest(manifest)
    paths = {"flash": flash, "uicr": uicr}
    for role, (name, address, expected_size) in BACKUP_FILES.items():
        record = document["files"].get(role)
        if not isinstance(record, dict):
            fail(f"backup manifest has no valid {role} record: {manifest}")
        expected_record = {
            "name": name,
            "address": f"0x{address:08x}",
            "size": expected_size,
        }
        for field, expected in expected_record.items():
            if record.get(field) != expected:
                fail(f"backup manifest has an invalid {role} {field}: {manifest}")
        digest = record.get("sha256")
        if (not isinstance(digest, str) or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)):
            fail(f"backup manifest has an invalid {role} SHA-256: {manifest}")
        path = paths[role]
        if path.name != name:
            fail(f"{role} backup must be named {name}: {path}")
        actual = backup_digest(path, expected_size)
        if actual != digest:
            fail(f"backup does not match the manifest: {path}")

    print(f"OK  {manifest.name} binds this flash/UICR pair")
    print(f"OK  {flash.name}  {document['files']['flash']['sha256']}")
    print(f"OK  {uicr.name}  {document['files']['uicr']['sha256']}")
    return flash, uicr


def validate_open_bootloader_uicr(path: Path) -> None:
    data = path.read_bytes()
    for offset, expected in OPEN_BOOTLOADER_UICR_WORDS.items():
        actual = struct.unpack_from("<I", data, offset)[0]
        if actual != expected:
            fail(
                f"UICR backup is not from the open bootloader layout: "
                f"+0x{offset:03x}=0x{actual:08x}, expected 0x{expected:08x}"
            )
    print("OK  UICR matches the open bootloader layout")


def run_openocd(args: argparse.Namespace, commands: list[str], *, target: bool) -> None:
    config_name = PROBES[args.probe]
    config = find_file(args.bundle, config_name)
    command = [find_openocd(args.openocd), "-f", str(config)]
    if target:
        command.extend(("-f", "target/nordic/nrf52.cfg", "-c", "adapter speed 400"))
    for item in commands:
        command.extend(("-c", item))
    print("Starting OpenOCD. Do not disconnect the wires until it exits.")
    subprocess.run(command, cwd=args.bundle, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--probe",
        choices=PROBES,
        required=True,
        help="programmer connected to the computer running this command",
    )
    parser.add_argument("--bundle", type=Path, default=Path.cwd(),
                        help="release folder (default: current directory)")
    parser.add_argument("--openocd", default="openocd",
                        help="OpenOCD executable name or path")
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("check", help="test the connection without erasing anything")
    install = actions.add_parser("install", help="erase stock firmware and install this project")
    install.add_argument("--erase-stock", "--erase-current", dest="erase_stock",
                         action="store_true",
                         help="confirm that all current Nordic firmware may be erased")
    actions.add_parser("flash-app", help="rewrite only apex-zmk.hex")
    backup = actions.add_parser("backup", help="save internal flash and chip settings")
    backup.add_argument("--output-dir", type=Path, default=Path.cwd())
    backup.add_argument("--overwrite", action="store_true",
                        help="replace an existing backup set in the output folder")
    actions.add_parser("update-mode", help="restart the keyboard as the APEXBOOT drive")
    repair = actions.add_parser(
        "repair-bootloader", help="rewrite the bootloader while preserving saved chip settings"
    )
    repair.add_argument("--flash-backup", type=Path, required=True)
    repair.add_argument("--uicr-backup", type=Path, required=True)
    repair.add_argument("--manifest", type=Path, required=True)
    repair.add_argument("--rewrite-bootloader", action="store_true",
                        help="confirm that the Nordic MBR and bootloader may be overwritten")
    restore = actions.add_parser("restore-backup", help="restore a complete matching backup pair")
    restore.add_argument("--flash-backup", type=Path, required=True)
    restore.add_argument("--uicr-backup", type=Path, required=True)
    restore.add_argument("--manifest", type=Path,
                         help=f"pair manifest (default: {BACKUP_MANIFEST} beside the backups)")
    restore.add_argument("--erase-current", action="store_true",
                         help="confirm that the current Nordic firmware may be erased")
    args = parser.parse_args()
    args.bundle = args.bundle.expanduser().resolve()
    if not args.bundle.is_dir():
        fail(f"release folder is missing: {args.bundle}")

    if args.action == "check":
        run_openocd(args, [
            "swd newdap nrf cpu -expected-id 0x2ba01477",
            "dap create nrf.dap -chain-position nrf.cpu",
            "target create nrf.cpu cortex_m -dap nrf.dap -defer-examine",
            "init",
            "set ctrl_ap_idr [nrf.dap apreg 1 0xfc]",
            "echo [format {CTRL-AP IDR: 0x%08x} $ctrl_ap_idr]",
            ("if {$ctrl_ap_idr != 0x02880000} {error [format "
             "{unexpected CTRL-AP IDR: 0x%08x; expected 0x02880000} $ctrl_ap_idr]}"),
            "shutdown",
        ], target=False)
    elif args.action == "install":
        if not args.erase_stock:
            fail("install erases all Nordic firmware; rerun with --erase-stock or --erase-current")
        verify_bundle(args.bundle)
        bootloader, app, uicr = (find_file(args.bundle, name) for name in FILES)
        run_openocd(args, [
            "init", "nrf52_recover", "reset halt",
            f"program {tcl_path(bootloader)} verify",
            f"program {tcl_path(app)} verify",
            f"flash write_image erase {tcl_path(uicr)} 0x10001000 bin",
            f"verify_image {tcl_path(uicr)} 0x10001000 bin",
            "reset run", "shutdown",
        ], target=True)
    elif args.action == "flash-app":
        verify_bundle(args.bundle)
        app = find_file(args.bundle, "apex-zmk.hex")
        run_openocd(args, ["init", "reset halt", f"program {tcl_path(app)} verify",
                           "reset run", "shutdown"], target=True)
    elif args.action == "backup":
        output = args.output_dir.expanduser().resolve()
        output.mkdir(parents=True, exist_ok=True)
        flash = output / BACKUP_FILES["flash"][0]
        uicr = output / BACKUP_FILES["uicr"][0]
        manifest = output / BACKUP_MANIFEST
        existing = [path for path in (flash, uicr, manifest) if path.exists()]
        if existing and not args.overwrite:
            names = ", ".join(path.name for path in existing)
            fail(f"backup output already exists ({names}); choose another folder or add --overwrite")

        token = uuid.uuid4().hex
        flash_temporary = output / f".{flash.name}.{token}.tmp"
        uicr_temporary = output / f".{uicr.name}.{token}.tmp"
        try:
            run_openocd(args, ["init", "reset halt",
                               f"dump_image {tcl_path(flash_temporary)} 0x00000000 0x80000",
                               f"dump_image {tcl_path(uicr_temporary)} 0x10001000 0x1000",
                               "reset run", "shutdown"], target=True)
            flash_digest = backup_digest(flash_temporary, BACKUP_FILES["flash"][2])
            uicr_digest = backup_digest(uicr_temporary, BACKUP_FILES["uicr"][2])
            if manifest.exists():
                manifest.unlink()
            flash_temporary.replace(flash)
            uicr_temporary.replace(uicr)
            write_backup_manifest(manifest, flash_digest, uicr_digest)
        finally:
            for temporary in (flash_temporary, uicr_temporary):
                if temporary.exists():
                    temporary.unlink()
        print(f"OK  {flash.name}  {flash_digest}")
        print(f"OK  {uicr.name}  {uicr_digest}")
        print(f"OK  {manifest.name} binds this flash/UICR pair")
    elif args.action == "update-mode":
        run_openocd(args, ["init", "mww 0x4000051C 0x57", "reset run", "shutdown"],
                    target=True)
    elif args.action == "repair-bootloader":
        if not args.rewrite_bootloader:
            fail("repair-bootloader overwrites the Nordic MBR and bootloader; "
                 "rerun with --rewrite-bootloader")
        verify_bundle(args.bundle)
        bootloader = find_file(args.bundle, "bootloader_mbr.hex")
        _, uicr = checked_backup_pair(
            args.flash_backup, args.uicr_backup, args.manifest
        )
        validate_open_bootloader_uicr(uicr)
        run_openocd(args, [
            "init", "reset halt", f"program {tcl_path(bootloader)} verify",
            "flash erase_sector 0 126 127",
            f"flash write_image erase {tcl_path(uicr)} 0x10001000 bin",
            f"verify_image {tcl_path(uicr)} 0x10001000 bin",
            "mww 0x4000051C 0x57", "reset run", "shutdown",
        ], target=True)
    else:
        if not args.erase_current:
            fail("restoring a backup erases all Nordic firmware; rerun with --erase-current")
        flash, uicr = checked_backup_pair(
            args.flash_backup, args.uicr_backup, args.manifest
        )
        run_openocd(args, [
            "init", "nrf52_recover", "reset halt",
            f"flash write_image erase {tcl_path(flash)} 0x00000000 bin",
            f"verify_image {tcl_path(flash)} 0x00000000 bin",
            f"flash write_image erase {tcl_path(uicr)} 0x10001000 bin",
            f"verify_image {tcl_path(uicr)} 0x10001000 bin",
            "reset run", "shutdown",
        ], target=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"programmer=FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
