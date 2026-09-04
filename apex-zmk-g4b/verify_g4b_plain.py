"""Read-only audit for the Adafruit-bootloader G4B application image."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
from pathlib import Path


UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_NOFLASH = 0x00000001
UF2_FLAG_FAMILYID = 0x00002000
UF2_FAMILY = 0x621E937A
SRAM_BASE = 0x20000000
SRAM_END = 0x20020000
AB_SLOT_SIZE = 0x71000


def numeric_define(path: Path, name: str, failures: list[str]) -> int:
    """Read a simple integer #define without invoking a C preprocessor."""
    if not path.is_file():
        failures.append(f"missing layout source {path}")
        return 0
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(
        rf"(?m)^\s*#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)u?\b",
        text,
    )
    if match is None:
        failures.append(f"{path} has no numeric #define {name}")
        return 0
    return int(match.group(1), 0)


def check_ab_layout_sources(failures: list[str]) -> None:
    """Keep app, verifier, and separately-built bootloader on one A/B map."""
    project = Path(__file__).resolve().parent.parent
    layout = Path(__file__).resolve().parent / "src" / "nor_layout_g4b.h"
    boot = project / "bootloader" / "apex_pro_mini_wl" / "ab_promote.c"
    app_b_addr = numeric_define(layout, "G4B_NOR_AB_IMAGE_ADDR", failures)
    app_b_size = numeric_define(layout, "G4B_NOR_AB_IMAGE_SIZE", failures)
    app_base = numeric_define(layout, "G4B_INTERNAL_APP_ADDR", failures)
    app_size = numeric_define(layout, "G4B_INTERNAL_APP_SIZE", failures)
    coredump = numeric_define(layout, "G4B_NOR_COREDUMP_ADDR", failures)
    nor_size = numeric_define(layout, "G4B_NOR_TOTAL_SIZE", failures)
    coredump_size = numeric_define(layout, "G4B_NOR_COREDUMP_SIZE", failures)
    boot_b_addr = numeric_define(boot, "AB_BIMG_ADDR", failures)
    boot_base = numeric_define(boot, "AB_APP_BASE", failures)
    boot_size = numeric_define(boot, "APEX_AB_APP_MAXLEN", failures)
    boot_version = numeric_define(boot, "AB_VERSION", failures)

    if app_b_size != app_size or app_size != AB_SLOT_SIZE:
        failures.append(
            "A/B image slot does not exactly mirror the 0x71000-byte app slot"
        )
    if app_b_addr + app_b_size != coredump:
        failures.append("A/B image slot does not end at the coredump boundary")
    if coredump + coredump_size != nor_size:
        failures.append("coredump ring does not end at the end of external NOR")
    if (boot_b_addr, boot_base, boot_size, boot_version) != (
        app_b_addr,
        app_base,
        app_size,
        2,
    ):
        failures.append("bootloader A/B layout/version differs from the app layout")


def strip_c_comments(text: str) -> str:
    """Remove comments while preserving line numbers for source checks."""
    output: list[str] = []
    index = 0
    while index < len(text):
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
            output.append("".join("\n" if char == "\n" else " "
                                  for char in text[index:end]))
            index = end
        elif text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
            output.append(" " * (end - index))
            index = end
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


def check_gpio_ownership(failures: list[str]) -> None:
    """Keep raw GPIO output writes in the board's pin-owner module."""
    source_dir = Path(__file__).resolve().parent / "src"
    register_names = ("OUTSET", "OUTCLR", "DIRSET", "DIRCLR", "PIN_CNF")
    offenders: list[str] = []

    for path in sorted(source_dir.glob("*.[ch]")):
        if path.name in {"pins_g4b.c", "pins_g4b.h"}:
            continue
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
        for line_number, line in enumerate(text.splitlines(), 1):
            if any(name in line for name in register_names):
                offenders.append(f"{path.name}:{line_number}")

    if offenders:
        failures.append(
            "raw GPIO output registers are used outside pins_g4b.c: "
            + ", ".join(offenders[:8])
        )


def check_ab_health_policy(failures: list[str]) -> None:
    """Keep charge-only VBUS out of the USB health requirement."""
    path = Path(__file__).resolve().parent / "src" / "link_g4b.c"
    text = strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
    match = re.search(
        r"static bool\s+ab_output_ready\s*\(void\)\s*\{(.*?)\n\}",
        text,
        re.DOTALL,
    )
    if match is None:
        failures.append("A/B output-health helper is missing")
        return

    body = re.sub(r"\s+", " ", match.group(1))
    required_expressions = (
        r"bool usb_required = s3_usb_is_powered\(\) && "
        r"\(g4b_mode_get\(\) != G4B_MODE_BT \|\| "
        r"zmk_endpoint_get_selected\(\)\.transport == ZMK_TRANSPORT_USB\);",
        r"return !usb_required \|\| zmk_usb_is_hid_ready\(\);",
    )
    for expression in required_expressions:
        if re.search(expression, body) is None:
            failures.append("A/B output-health policy has changed")
    if "zmk_usb_get_status() == USB_DC_CONFIGURED" in body:
        failures.append("A/B health rejects a configured host after USB suspend")

    note = re.search(
        r"static void\s+ab_note_health\s*\([^)]*\)\s*\{(.*?)\n\}",
        text,
        re.DOTALL,
    )
    if note is None or "ab_output_ready()" not in note.group(1):
        failures.append("A/B scanner-health gate does not use the output policy")


def read_config(path: Path) -> dict[str, str]:
    config: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.fullmatch(r"CONFIG_([A-Za-z0-9_]+)=(.*)", line)
        if match:
            config[match.group(1)] = match.group(2)
    return config


def check_disabled_diagnostics(config: dict[str, str], symbols: str,
                               failures: list[str]) -> None:
    """Make sure disabled diagnostics do not leave polling threads behind."""
    forbidden: list[str] = []
    if config.get("APEX_G4B_USB_KICK") != "y":
        forbidden.append("g4b_usb_watch_thread")
    if (config.get("APEX_G4B_UART_EMIT") != "y" and
            config.get("APEX_G4B_EVIDENCE_USB") != "y"):
        forbidden.extend(("g4b_ab_status_tid", "g4b_coredump_emit_tid"))

    for name in forbidden:
        if re.search(rf"(?m)^[0-9a-fA-F]+\s+\S\s+.*{re.escape(name)}$", symbols):
            failures.append(f"disabled diagnostic thread remains in the image: {name}")


def parse_int(value: str | None, name: str, failures: list[str]) -> int:
    try:
        return int(value or "", 0)
    except ValueError:
        failures.append(f"{name} is missing or invalid: {value!r}")
        return 0


def check_elf(elf_path: Path, app_base: int, app_end: int,
              reset: int, failures: list[str]) -> None:
    data = elf_path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        failures.append("zmk.elf is not a little-endian ELF32 image")
        return

    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    entry, phoff, phentsize, phnum = header[4], header[5], header[9], header[10]
    if entry != reset:
        failures.append(
            f"ELF entry 0x{entry:08x} differs from reset vector 0x{reset:08x}"
        )

    load_count = 0
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + 32 > len(data):
            failures.append("ELF program-header table is truncated")
            return
        p_type, _off, vaddr, paddr, filesz, memsz, _flags, _align = (
            struct.unpack_from("<IIIIIIII", data, offset)
        )
        if p_type != 1:
            continue
        load_count += 1
        if filesz and paddr < SRAM_BASE and not (
            app_base <= paddr and paddr + filesz <= app_end
        ):
            failures.append(
                f"flash LOAD 0x{paddr:08x}+0x{filesz:x} leaves the app partition"
            )
        if vaddr >= SRAM_BASE and not (
            SRAM_BASE <= vaddr and vaddr + max(memsz, 1) <= SRAM_END
        ):
            failures.append(
                f"RAM LOAD 0x{vaddr:08x}+0x{memsz:x} leaves nRF52833 SRAM"
            )
    if load_count == 0:
        failures.append("zmk.elf has no LOAD segments")


def check_hex(path: Path, payload: bytes, app_base: int,
              app_end: int, failures: list[str]) -> None:
    """Strictly parse copied Intel HEX and compare its flash image to BIN."""
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except UnicodeDecodeError:
        failures.append("plain HEX is not ASCII")
        return

    if not lines:
        failures.append("plain HEX is empty")
        return

    image = bytearray(b"\xff" * len(payload))
    written = bytearray(len(payload))
    base = 0
    eof_seen = False
    start_record_seen = False

    for line_number, line in enumerate(lines, 1):
        if eof_seen:
            failures.append(f"plain HEX has a record after EOF at line {line_number}")
            break

        match = re.fullmatch(r":([0-9A-Fa-f]+)", line)
        if match is None or len(match.group(1)) % 2:
            failures.append(f"plain HEX line {line_number} has invalid record syntax")
            continue

        try:
            record = bytes.fromhex(match.group(1))
        except ValueError:
            failures.append(f"plain HEX line {line_number} has invalid hex digits")
            continue
        if len(record) < 5:
            failures.append(f"plain HEX line {line_number} is shorter than a record")
            continue

        count = record[0]
        if len(record) != count + 5:
            failures.append(
                f"plain HEX line {line_number} length disagrees with byte count"
            )
            continue
        if sum(record) & 0xFF:
            failures.append(f"plain HEX line {line_number} has an invalid checksum")
            continue

        address = (record[1] << 8) | record[2]
        record_type = record[3]
        data = record[4:-1]

        if record_type == 0x00:
            if address + count > 0x10000:
                failures.append(
                    f"plain HEX data record at line {line_number} crosses a 64-KiB window"
                )
                continue
            absolute = base + address
            end = absolute + count
            if not (app_base <= absolute and end <= app_end):
                failures.append(
                    f"plain HEX data 0x{absolute:08x}+0x{count:x} "
                    "leaves the app partition"
                )
                continue
            if end > app_base + len(payload):
                failures.append(
                    f"plain HEX data 0x{absolute:08x}+0x{count:x} "
                    "extends beyond zmk.bin"
                )
                continue

            start = absolute - app_base
            if any(written[start : start + count]):
                failures.append(
                    f"plain HEX data at line {line_number} overlaps an earlier record"
                )
                continue
            image[start : start + count] = data
            written[start : start + count] = b"\x01" * count
        elif record_type == 0x01:
            if count != 0 or address != 0:
                failures.append(f"plain HEX EOF record at line {line_number} is malformed")
            else:
                eof_seen = True
        elif record_type == 0x02:
            if count != 2 or address != 0:
                failures.append(
                    f"plain HEX extended-segment record at line {line_number} is malformed"
                )
            else:
                base = int.from_bytes(data, "big") << 4
        elif record_type == 0x03:
            if count != 4 or address != 0:
                failures.append(
                    f"plain HEX start-segment record at line {line_number} is malformed"
                )
            elif start_record_seen:
                failures.append("plain HEX has more than one start-address record")
            else:
                start_record_seen = True
        elif record_type == 0x04:
            if count != 2 or address != 0:
                failures.append(
                    f"plain HEX extended-linear record at line {line_number} is malformed"
                )
            else:
                base = int.from_bytes(data, "big") << 16
        elif record_type == 0x05:
            if count != 4 or address != 0:
                failures.append(
                    f"plain HEX start-linear record at line {line_number} is malformed"
                )
            elif start_record_seen:
                failures.append("plain HEX has more than one start-address record")
            else:
                start_record_seen = True
        else:
            failures.append(
                f"plain HEX line {line_number} uses unsupported record type "
                f"0x{record_type:02x}"
            )

    if not eof_seen:
        failures.append("plain HEX has no valid EOF record")
    if image != payload:
        failures.append("plain HEX does not exactly reconstruct zmk.bin")


def check_uf2(path: Path, payload: bytes, app_base: int,
              app_end: int, failures: list[str], *, strict: bool = False) -> None:
    data = path.read_bytes()
    if not data or len(data) % 512:
        failures.append(f"UF2 size {len(data)} is not a nonzero multiple of 512")
        return

    seen: set[int] = set()
    declared_blocks: int | None = None
    strict_image = bytearray()
    strict_next_target = app_base
    physical_blocks = len(data) // 512
    for offset in range(0, len(data), 512):
        block = data[offset : offset + 512]
        magic0, magic1, flags, target, size, number, total, family = (
            struct.unpack_from("<IIIIIIII", block, 0)
        )
        end_magic = struct.unpack_from("<I", block, 508)[0]
        if (magic0, magic1, end_magic) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END):
            failures.append(f"UF2 block {offset // 512} has invalid magic")
            return
        if family != UF2_FAMILY:
            failures.append(f"UF2 block {number} has family 0x{family:08x}")
        if size > 476 or not (app_base <= target and target + size <= app_end):
            failures.append(f"UF2 block {number} leaves the app partition")
            continue
        if strict:
            physical_number = offset // 512
            # The build's uf2conv.py emits exactly FAMILYID, and the target
            # Adafruit loader rejects blocks without FAMILYID, with NOFLASH,
            # with a payload other than 256 bytes, or on an unaligned target.
            if not flags & UF2_FLAG_FAMILYID:
                failures.append(
                    f"UF2 block {number} does not set the FAMILYID flag"
                )
            if flags & UF2_FLAG_NOFLASH:
                failures.append(f"UF2 block {number} sets the NOFLASH flag")
            unexpected_flags = flags & ~(UF2_FLAG_FAMILYID | UF2_FLAG_NOFLASH)
            if unexpected_flags:
                failures.append(
                    f"UF2 block {number} has unexpected flags 0x{unexpected_flags:08x}; "
                    f"the build converter emits exactly 0x{UF2_FLAG_FAMILYID:08x}"
                )
            if size != 256:
                failures.append(
                    f"UF2 block {number} has payload size {size}, expected 256"
                )
            if target & 0xFF:
                failures.append(
                    f"UF2 block {number} target 0x{target:08x} is not 256-byte aligned"
                )
            if number != physical_number:
                failures.append(
                    f"UF2 physical block {physical_number} is numbered {number}"
                )
            if total != physical_blocks:
                failures.append(
                    f"UF2 block {number} declares {total} blocks, "
                    f"but the file contains {physical_blocks}"
                )
            if target != strict_next_target:
                failures.append(
                    f"UF2 block {number} targets 0x{target:08x}, "
                    f"expected sequential address 0x{strict_next_target:08x}"
                )
            else:
                strict_image.extend(block[32 : 32 + size])
                strict_next_target += size
        start = target - app_base
        overlap = max(0, min(size, len(payload) - start))
        if block[32 : 32 + overlap] != payload[start : start + overlap]:
            failures.append(f"UF2 block {number} differs from zmk.bin")
        if any(byte != 0xFF for byte in block[32 + overlap : 32 + size]):
            failures.append(f"UF2 block {number} has non-erased padding beyond zmk.bin")
        seen.add(number)
        declared_blocks = total if declared_blocks is None else declared_blocks
        if total != declared_blocks:
            failures.append("UF2 blocks disagree on their total count")
    if declared_blocks is not None and not (
        declared_blocks == len(seen) and seen == set(range(len(seen)))
    ):
        failures.append("UF2 block numbering is incomplete or duplicated")
    if strict:
        if len(strict_image) < len(payload):
            failures.append("UF2 does not completely cover zmk.bin")
        elif strict_image[:len(payload)] != payload:
            failures.append("UF2 sequential reconstruction differs from zmk.bin")
        elif any(byte != 0xFF for byte in strict_image[len(payload):]):
            failures.append("UF2 sequential reconstruction has non-erased trailing bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--zephyr-base", type=Path, required=True)
    parser.add_argument("--expect-stage", type=int, choices=range(8), required=True)
    scanner_profile = parser.add_mutually_exclusive_group()
    scanner_profile.add_argument("--stop1-canary", action="store_true")
    scanner_profile.add_argument(
        "--wireless-idle", "--mode3-canary", dest="wireless_idle", action="store_true"
    )
    parser.add_argument("--ab-v2", "--ab-canary", dest="ab_v2", action="store_true")
    parser.add_argument("--ab-crash-test", action="store_true")
    args = parser.parse_args()

    if args.ab_crash_test and not args.ab_v2:
        parser.error("--ab-crash-test requires --ab-v2")

    zephyr = args.build_dir / "zephyr"
    config_path = zephyr / ".config"
    dts_path = zephyr / "zephyr.dts"
    elf_path = zephyr / "zmk.elf"
    payload_path = zephyr / "zmk.bin"
    artifact_bin = args.artifact_dir / "apex-zmk-g4b.plain.bin"
    artifact_hex = args.artifact_dir / "apex-zmk-g4b.plain.hex"
    artifact_elf = args.artifact_dir / "apex-zmk-g4b.plain.elf"
    artifact_config = args.artifact_dir / "apex-zmk-g4b.plain.config"
    artifact_symbols = args.artifact_dir / "apex-zmk-g4b.plain.symbols.txt"
    artifact_uf2 = args.artifact_dir / "apex-zmk-g4b.plain.uf2"
    required = (
        config_path,
        dts_path,
        elf_path,
        payload_path,
        artifact_bin,
        artifact_hex,
        artifact_elf,
        artifact_config,
        artifact_symbols,
    )
    if args.wireless_idle:
        required += (artifact_uf2,)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("verification=FAIL")
        for path in missing:
            print(f"  reason=missing {path}")
        return 1

    failures: list[str] = []
    check_ab_layout_sources(failures)
    check_gpio_ownership(failures)
    check_ab_health_policy(failures)
    config = read_config(config_path)
    payload = payload_path.read_bytes()
    copied = artifact_bin.read_bytes()
    copied_elf = artifact_elf.read_bytes()
    copied_config = artifact_config.read_bytes()
    symbols_text = artifact_symbols.read_text(encoding="ascii", errors="replace")
    check_disabled_diagnostics(config, symbols_text, failures)
    dts = dts_path.read_text(encoding="utf-8", errors="replace")
    app_base = parse_int(config.get("FLASH_LOAD_OFFSET"), "FLASH_LOAD_OFFSET", failures)
    app_size = parse_int(config.get("FLASH_LOAD_SIZE"), "FLASH_LOAD_SIZE", failures)
    app_end = app_base + app_size

    if config.get("APEX_G4B_STAGE") != str(args.expect_stage):
        failures.append(
            f"built stage is {config.get('APEX_G4B_STAGE')!r}, expected {args.expect_stage}"
        )
    if args.ab_v2:
        expected_ab = {
            "APEX_G4B_AB_ROLLBACK": "y",
            "APEX_G4B_AB_AUTOSTAGE": "y",
        }
        for name, value in expected_ab.items():
            if config.get(name) != value:
                failures.append(
                    f"A/B v2 CONFIG_{name} is {config.get(name)!r}, "
                    f"expected {value!r}"
                )
        for symbol in (
            "g4b_ab_boot_pending",
            "g4b_ab_mark_healthy",
            "g4b_ab_stage_erase",
            "g4b_ab_stage_write",
            "g4b_ab_commit",
        ):
            if re.search(
                rf"(?m)^[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\S\s+{re.escape(symbol)}$",
                symbols_text,
            ) is None:
                failures.append(f"A/B v2 symbol is missing: {symbol}")
        crash_enabled = config.get("APEX_G4B_AB_CRASHTEST") == "y"
        if args.ab_crash_test and not crash_enabled:
            failures.append("A/B crash-test build does not enable its early fault")
        elif not args.ab_crash_test and crash_enabled:
            failures.append("A/B v2 profile unexpectedly enables the destructive crash test")
    elif config.get("APEX_G4B_AB_ROLLBACK") == "y":
        failures.append("build enables A/B rollback without the A/B v2 verifier profile")
    if len(payload) < 8 or len(payload) > app_size:
        failures.append(f"payload size {len(payload)} exceeds app size {app_size}")
        payload_sp = payload_reset = 0
    else:
        payload_sp, payload_reset = struct.unpack_from("<II", payload, 0)
        if not (SRAM_BASE <= payload_sp <= SRAM_END and payload_sp % 8 == 0):
            failures.append(f"invalid initial stack pointer 0x{payload_sp:08x}")
        if not (payload_reset & 1 and app_base <= (payload_reset & ~1) < app_end):
            failures.append(f"invalid reset vector 0x{payload_reset:08x}")

    if copied != payload:
        failures.append("plain artifact does not exactly match zmk.bin")
    if copied_elf != elf_path.read_bytes():
        failures.append("plain ELF artifact does not exactly match zmk.elf")
    if copied_config != config_path.read_bytes():
        failures.append("plain config artifact does not exactly match .config")

    partition = re.search(r"partition@1000\s*\{(.*?)\n\s*\};", dts, re.S)
    if app_base != 0x1000 or app_size != 0x71000:
        failures.append(
            f"app partition is 0x{app_base:x}+0x{app_size:x}, expected 0x1000+0x71000"
        )
    if partition is None or "reg = < 0x1000 0x71000 >;" not in partition.group(1):
        failures.append("devicetree code partition is not 0x1000+0x71000")

    if config.get("ZMK_USB") == "y" and args.expect_stage == 3:
        expected = {
            "APEX_G4B_KSCAN_INGEST": "y",
            "APEX_G4B_WATCHDOG": "y",
            "APEX_G4B_IDLE_AFTER_MS": "5000",
            "APEX_G4B_IDLE_POLL_MS": "200",
            "APEX_G4B_SLEEP_MS": "0",
        }
        for name, value in expected.items():
            if config.get(name) != value:
                failures.append(f"CONFIG_{name} is {config.get(name)!r}, expected {value!r}")
        if "regulator-initial-mode = < 0x1 >;" not in dts:
            failures.append("reg1 is not configured for nRF DC/DC mode")

        usb_dc = args.zephyr_base / "drivers" / "usb" / "device" / "usb_dc_nrfx.c"
        usbd_common = (args.zephyr_base / "drivers" / "usb" / "common" /
                       "nrf_usbd_common" / "nrf_usbd_common.c")
        if not usb_dc.is_file():
            failures.append(f"missing Zephyr USB source {usb_dc}")
        else:
            text = usb_dc.read_text(encoding="utf-8", errors="replace")
            if not ("NRFX_POWER_USB_STATE_READY" in text and
                    "NRFX_POWER_USB_EVT_READY" in text and
                    text.index("NRFX_POWER_USB_STATE_READY") >
                    text.index("nrfx_power_usbevt_enable")):
                failures.append("Zephyr USBPWRRDY synthesis patch is missing")
        if not usbd_common.is_file():
            failures.append(f"missing Zephyr USBD source {usbd_common}")
        else:
            text = usbd_common.read_text(encoding="utf-8", errors="replace")
            gate = text.find("POWER_USBREGSTATUS_OUTPUTRDY_Msk")
            pullup = text.find("NRF_USBD->USBPULLUP = 1")
            if gate < 0 or pullup < 0 or gate > pullup:
                failures.append("Zephyr USB OUTPUTRDY-before-pullup patch is missing")

        # Battery-only boots must leave the USBD/HFXO request off, while a
        # cable already present across the Adafruit-loader handoff must still
        # enumerate. Keep both the live ZMK tree and its reproducibility patch
        # tied to the level-aware implementation.
        zmk_usb = args.zephyr_base.parent / "app" / "src" / "usb.c"
        usb_patch = Path(__file__).parent / "patches" / "0002-zmk-usb-device-next-migration.patch"
        usb_power_tokens = (
            "nrf_power_usbregstatus_vbusdet_get(NRF_POWER)",
            "zmk_usb_enable_if_powered",
            "case USBD_MSG_VBUS_READY",
        )
        for path, label in ((zmk_usb, "ZMK USB source"),
                            (usb_patch, "ZMK USB migration patch")):
            if not path.is_file():
                failures.append(f"missing {label} {path}")
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for token in usb_power_tokens:
                if token not in text:
                    failures.append(f"{label} is missing battery USB gate token {token!r}")

        # The RGB controller is unpowered after the idle fade. SPIM2 and its
        # bit-banged CS must be parked too, then restored before rail-up.
        rgb_source = Path(__file__).parent / "src" / "rgb_g4b.c"
        if not rgb_source.is_file():
            failures.append(f"missing RGB source {rgb_source}")
        else:
            text = rgb_source.read_text(encoding="utf-8", errors="replace")
            blank_start = text.find("void g4b_rgb_set_blanked(bool blank)")
            blank_end = text.find("bool g4b_rgb_fading(void)", blank_start)
            if blank_start < 0 or blank_end < 0:
                failures.append("RGB blanking implementation is missing")
                blank_body = ""
            else:
                blank_body = text[blank_start:blank_end]
            park_order = (
                "spim2_write(dark, sizeof(dark));",
                "spim2_park();",
                "g4b_rgb_rail_down();",
                "g4b_rgb_cs_park();",
            )
            wake_order = (
                "g4b_rgb_cs_init();",
                "spim2_enable();",
                "g4b_rgb_rail_up();",
                "g4b_rgb_bringup();",
            )
            for name, tokens in (("park", park_order), ("wake", wake_order)):
                positions = [blank_body.find(token) for token in tokens]
                if any(position < 0 for position in positions) or positions != sorted(positions):
                    failures.append(f"RGB {name} sequence is missing or out of order")

        for symbol in ("zmk_usb_enable_if_powered", "g4b_rgb_cs_park",
                       "g4b_wdt_keyboard_heartbeat"):
            if re.search(rf"(?m)^[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\S\s+{symbol}$",
                         symbols_text) is None:
                failures.append(f"power-saving symbol is missing: {symbol}")

    stop1_ms = parse_int(
        config.get("APEX_G4B_STM32_STOP1_IDLE_MS"),
        "APEX_G4B_STM32_STOP1_IDLE_MS",
        failures,
    )
    if args.stop1_canary:
        if stop1_ms == 0:
            failures.append("STOP1 canary requested but its idle threshold is zero")
    elif stop1_ms != 0:
        failures.append(f"production build enables the failed STOP1 canary ({stop1_ms} ms)")

    mode3_period = parse_int(
        config.get("APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS"),
        "APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS",
        failures,
    )
    if args.wireless_idle:
        if mode3_period != 50:
            failures.append(
                f"wireless-idle period is {mode3_period}, expected the reviewed 50 ms"
            )
        long_mode3_period = parse_int(
            config.get("APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS"),
            "APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS",
            failures,
        )
        long_mode3_after = parse_int(
            config.get("APEX_G4B_STM32_LONG_IDLE_AFTER_MS"),
            "APEX_G4B_STM32_LONG_IDLE_AFTER_MS",
            failures,
        )
        if long_mode3_period != 255:
            failures.append(
                "mode-3 long-idle period is "
                f"{long_mode3_period}, expected the one-byte maximum 255 ms"
            )
        if long_mode3_after != 60000:
            failures.append(
                "mode-3 long-idle threshold is "
                f"{long_mode3_after}, expected the reviewed 60000 ms"
            )
        if (mode3_period is not None and long_mode3_period is not None and
                long_mode3_period <= mode3_period):
            failures.append("mode-3 long-idle period must exceed the first idle period")
        expected_mode3 = {
            "APEX_G4B_STAGE": "3",
            "ZMK_USB": "y",
            "ZMK_BLE": "y",
            "APEX_G4B_KSCAN_INGEST": "y",
            "APEX_G4B_SLEEP_MS": "0",
            "APEX_G4B_STM32_STOP1_IDLE_MS": "0",
        }
        for name, value in expected_mode3.items():
            if config.get(name) != value:
                failures.append(
                    f"wireless-idle CONFIG_{name} is {config.get(name)!r}, "
                    f"expected {value!r}"
                )
        if config.get("APEX_G4B_KBD_CAPTURE") == "y":
            failures.append("wireless-idle profile enables the non-ingesting keyboard capture path")
        reset_ms = parse_int(
            config.get("APEX_G4B_STM32_RESET_MS"),
            "APEX_G4B_STM32_RESET_MS",
            failures,
        )
        if reset_ms != 250:
            failures.append(
                f"wireless-idle scanner recovery reset is {reset_ms} ms, "
                "expected exactly 250 ms"
            )
        required_symbols = {
            "g4b_mode3_entry_attempts",
            "g4b_mode3_entries",
            "g4b_mode3_attn_resumes",
            "g4b_mode3_fast_overrides",
            "g4b_mode3_failures",
            "g4b_mode3_recoveries",
            "g4b_mode3_expected_period",
            "g4b_mode3_last_reply",
            "g4b_mode3_last_fault",
            "g4b_mode3_failure_fault",
            "g4b_mode3_last_slow_residency_ms",
            "g4b_mode3_long_entry_attempts",
            "g4b_mode3_long_entries",
        }
        for symbol in sorted(required_symbols):
            if re.search(rf"(?m)^[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\S\s+{re.escape(symbol)}$",
                         symbols_text) is None:
                failures.append(f"mode-3 telemetry symbol is missing: {symbol}")
    elif mode3_period != 0:
        failures.append(
            f"build enables scanner idle throttling without the wireless-idle verifier profile ({mode3_period})"
        )

    if len(payload) > AB_SLOT_SIZE and config.get("APEX_G4B_AB_ROLLBACK") == "y":
        failures.append(
            "A/B rollback is enabled although the payload exceeds its 452 KiB B slot"
        )

    check_elf(elf_path, app_base, app_end, payload_reset, failures)
    check_hex(artifact_hex, payload, app_base, app_end, failures)
    if artifact_uf2.is_file():
        check_uf2(
            artifact_uf2,
            payload,
            app_base,
            app_end,
            failures,
            strict=args.wireless_idle,
        )

    if failures:
        print("verification=FAIL")
        for failure in failures:
            print(f"  reason={failure}")
        return 1

    digest = hashlib.sha256(payload).hexdigest()
    print("verification=PASS")
    print(f"payload={payload_path}")
    print(f"payload_size={len(payload)}")
    print(f"payload_sha256={digest}")
    print(f"app_partition=0x{app_base:x}+0x{app_size:x}")
    print(f"payload_sp=0x{payload_sp:08x}")
    print(f"payload_reset=0x{payload_reset:08x}")
    print(f"uf2={'checked' if artifact_uf2.is_file() else 'not-present'}")
    if args.wireless_idle:
        print(f"mode3_period_ms={mode3_period}")
        print(f"mode3_long_period_ms={long_mode3_period}")
        print(f"mode3_long_after_ms={long_mode3_after}")
        print("mode3_symbols=checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
