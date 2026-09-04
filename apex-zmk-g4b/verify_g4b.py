"""Independent audit of the APX-ZMK-G4A candidate.

Checks the generated Kconfig, the generated devicetree, the ELF load segments,
the linked symbol table, the wrapper disassembly, and the packaged vendor image
against the G4A gate contract. It reads only build outputs; it never rebuilds,
uploads, or flashes.

Exit status is non-zero on the first failed requirement.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build" / "zephyr"
ARTIFACTS = ROOT / "artifacts"
ZEPHYR_BASE = ROOT.parent / "zmk-upstream" / "zephyr"

IMAGE = ARTIFACTS / "apex-zmk-g4b.vendor.bin"
PAYLOAD = BUILD / "zmk.bin"
FAILSAFE = ARTIFACTS / "failsafe.bin"
FAILSAFE_DISASM = ARTIFACTS / "failsafe.disasm.txt"
SYMBOLS = ARTIFACTS / "zmk.symbols.txt"
SEGMENTS = ARTIFACTS / "zmk.segments.txt"
DOTCONFIG = BUILD / ".config"
ZEPHYR_DTS = BUILD / "zephyr.dts"


def configure_paths(build_dir: Path, artifact_dir: Path,
                    zephyr_base: Path) -> None:
    """Point every derived input at the outputs selected by the caller.

    ``build_dir`` is the CMake build root (the directory containing
    ``zephyr/``), matching build_g4b.py and verify_g4b_plain.py. Keeping path
    selection here prevents the wrapper verifier from silently inspecting
    stale repo-local outputs when the source tree uses the shared work area.
    """
    global BUILD, ARTIFACTS, ZEPHYR_BASE
    global IMAGE, PAYLOAD, FAILSAFE, FAILSAFE_DISASM
    global SYMBOLS, SEGMENTS, DOTCONFIG, ZEPHYR_DTS

    BUILD = build_dir.resolve() / "zephyr"
    ARTIFACTS = artifact_dir.resolve()
    ZEPHYR_BASE = zephyr_base.resolve()
    IMAGE = ARTIFACTS / "apex-zmk-g4b.vendor.bin"
    PAYLOAD = BUILD / "zmk.bin"
    FAILSAFE = ARTIFACTS / "failsafe.bin"
    FAILSAFE_DISASM = ARTIFACTS / "failsafe.disasm.txt"
    SYMBOLS = ARTIFACTS / "zmk.symbols.txt"
    SEGMENTS = ARTIFACTS / "zmk.segments.txt"
    DOTCONFIG = BUILD / ".config"
    ZEPHYR_DTS = BUILD / "zephyr.dts"

IMAGE_BASE = 0x1C000
IMAGE_SIZE = 0x4B000


def layout() -> tuple[int, int, int]:
    """(payload slot size, wrapper address, cookie address) from the build.

    These were hardcoded at 0x30000 / 0x4C000 / 0x4D000, which silently assumed
    one layout. The wrapper sits immediately above the code partition, so its
    address is a consequence of FLASH_LOAD_SIZE - deriving it means a build that
    moves the wrapper without moving the partition (or vice versa) fails here
    instead of producing an image whose reset vector points at padding.
    """
    load_size = int(read_config().get("FLASH_LOAD_SIZE", "0x30000"), 16)
    wrapper = IMAGE_BASE + load_size
    return load_size, wrapper, wrapper + 0x1000
TRAILER_OFFSET = IMAGE_SIZE - 4
CRC32_RESIDUE = 0x2144DF1C
WDT_RELOAD_MAGIC = struct.pack("<I", 0x6E524635)

# The frozen stock configuration prefix, as reviewed for the G4B-2 gate.
PREFIX_FRAMES = 59
PREFIX_SHA256 = "d2c49d198fcd11f0f4584d46ddb8c6193bed8a359680a7823d69adedf2898fe5"
# Retired flash cookie values. They must not reappear in the wrapper.
G4A_COOKIE_MAGIC = 0x37413447
G4A2_RETAINED_MAGIC = 0x47
BEACON_COOKIE_MAGIC = 0x37454C42
# The live one-shot: POWER->GPREGRET2, low byte only.
GPREGRET2 = 0x40000520
RETAINED_COOKIE_MAGIC = 0x42
SRAM_LOW = 0x20003000
SRAM_HIGH = 0x2001FFFF
# The advertised name is how an increment is identified over the air, so it must
# track the stage rather than being pinned to one value. A stage-2 image
# advertising as G4B1 would make an OTA observation say the wrong thing about
# what is running.
def ble_name_for(stage: int, kscan_ingest: bool = False,
                 studio: bool = False, persistent: bool = False) -> str:
    """The K suffix marks the ingest build.

    Without it, the stage-3 image and its ingest-enabled follow-up would
    advertise the same name, which is exactly the ambiguity this check exists to
    prevent: an over-the-air observation could not say which of the two is
    running, and they behave very differently. S marks the ZMK Studio build for
    the same reason.
    """
    return (f"APX-ZMK-G4B{stage}" + ("K" if kscan_ingest else "")
            + ("S" if studio else "") + ("P" if persistent else ""))

# Every symbol here must resolve to "not set" or be absent from the generated
# Kconfig. A symbol that is absent had its dependencies removed, which is a
# stronger result than an explicit n.
FORBIDDEN_CONFIGS = (
    "ZMK_USB",
    "USB_DEVICE_STACK",
    "USB_DEVICE_DRIVER",
    "SETTINGS",
    "BT_SETTINGS",
    "NVS",
    "SETTINGS_NVS",
    "FLASH",
    "FLASH_MAP",
    "FLASH_PAGE_LAYOUT",
    "SOC_FLASH_NRF",
    "SPI",
    "SPI_NRFX",
    "NRFX_SPIM3",
    "GPIO",
    "PINCTRL",
    "WATCHDOG",
    "NRFX_WDT0",
    "REBOOT",
    "POWEROFF",
    "SHELL",
    "SERIAL",
    "CONSOLE",
    "LOG",
    "PRINTK",
    "ZMK_SLEEP",
    "ZMK_PM",
    "ZMK_PM_SOFT_OFF",
    "APEX_STM32_KSCAN",
    "APEX_RECOVERY",
    "NRF_APPROTECT_LOCK",
    "BT_DEVICE_NAME_DYNAMIC",
    # Both of these make Zephyr's nRF SoC init WRITE UICR and then self-reset,
    # which is the one operation this project treats as non-negotiable. The
    # segment check does not catch them: that is a runtime NVMC write, not a
    # load segment, so nothing else here would notice.
    #
    # They are also unnecessary. Stock already programmed PSELRESET[0]/[1] to
    # 0x12 and cleared NFCPINS.PROTECT, and UICR survives an application
    # reflash - so we inherit both and must not redo them.
    "GPIO_AS_PINRESET",
    "NFCT_PINS_AS_GPIOS",
)

REQUIRED_CONFIGS = {
    "APEX_G4B_SPIM_KSCAN": "y",
    "APEX_G4B_UART_EVIDENCE": "y",
    "ZMK_KSCAN_EVENT_QUEUE_SIZE": "64",
    "ZMK_BLE": "y",
    "BT_PERIPHERAL": "y",
    "USE_DT_CODE_PARTITION": "y",
    "CLOCK_CONTROL_NRF_K32SRC_RC": "y",
    "FLASH_LOAD_OFFSET": "0x1c000",
    "SRAM_BASE_ADDRESS": "0x20003000",
    "SRAM_SIZE": "116",
}

# Substrings that must not appear in the payload's symbol table. These catch a
# driver that got linked in without its Kconfig symbol being obvious.
FORBIDDEN_SYMBOLS = (
    "nrfx_wdt",
    "nrfx_spim",
    "nrf_nvmc",
    "flash_nrf",
    "soc_flash",
    "nvs_",
    "settings_",
    "usb_dc_",
    "usb_dev",
    "z_impl_flash_",
    "apex_stm32",
    "apex_g4a_noio",
)

failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def read_config() -> dict[str, str]:
    values: dict[str, str] = {}
    for line in DOTCONFIG.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, _, value = line.partition("=")
            values[key[len("CONFIG_") :]] = value
    return values


# Stage 6 is the first stage with persistent storage, so it needs the flash
# stack every other stage forbids. The exemption is narrow and paid for by the
# partition-placement checks in check_devicetree(): being allowed to write flash
# is only safe because WHERE it may write is pinned.
STAGE6_ALLOWED_CONFIGS = (
    "FLASH", "FLASH_MAP", "FLASH_PAGE_LAYOUT", "NVS", "SOC_FLASH_NRF",
    "NRFX_NVMC", "SOC_FLASH_NRF_RADIO_SYNC_TICKER", "CRC",
)
# Stage 7 is stage 6 plus the settings layer and BLE bond storage.
STAGE7_ALLOWED_CONFIGS = STAGE6_ALLOWED_CONFIGS + ("SETTINGS", "SETTINGS_NVS", "BT_SETTINGS")
STAGE7_ALLOWED_SYMBOLS = ("nrf_nvmc", "flash_nrf", "soc_flash", "nvs_",
                          "z_impl_flash_", "settings_")

# A build that carries ZMK Studio over USB CDC-ACM needs the USB stack and
# Zephyr serial. Keyed off CONFIG_ZMK_USB, the same way the storage exemptions
# are keyed off NVS/SETTINGS rather than a stage number.
USB_ALLOWED_CONFIGS = ("ZMK_USB", "USB_DEVICE_STACK", "USB_DEVICE_DRIVER",
                       "SERIAL", "CONSOLE")
USB_ALLOWED_SYMBOLS = ("usb_dc_", "usb_dev")
STAGE6_ALLOWED_SYMBOLS = ("nrf_nvmc", "flash_nrf", "soc_flash", "nvs_",
                          "z_impl_flash_")

# Measured erased by the G4B-4 survey, above the CRC-covered image, with a guard
# page below and the top two pages of flash left alone. See g4b6.overlay.
STORAGE_OFFSET = 0x7A000
STORAGE_SIZE = 0x4000
IMAGE_END = 0x67000


def check_config() -> None:
    config = read_config()
    stage = int(config.get("APEX_G4B_STAGE", "-1"))
    # Any build with the flash stack enabled, whatever its stage.
    has_storage = config.get("NVS") == "y" or config.get("SETTINGS") == "y"
    has_usb = config.get("ZMK_USB") == "y"
    for symbol in FORBIDDEN_CONFIGS:
        if has_storage and symbol in STAGE7_ALLOWED_CONFIGS:
            continue
        if has_usb and symbol in USB_ALLOWED_CONFIGS:
            continue
        require(symbol not in config, f"config {symbol} is enabled as {config.get(symbol)!r}")

    # A deliberately BLE-less control build. Every USB implementation on this
    # board is identical (loader, stock app, Zephyr), so the remaining variable
    # is the environment the code runs in - and BLE being up is the last
    # difference between our payload and the loader that enumerates. Such a
    # build has no BLE name, no peripheral role and no bond storage, so the
    # BLE-shaped requirements below do not apply to it. Everything else still
    # does: this narrows the checks, it does not switch them off.
    no_ble = config.get("ZMK_BLE") != "y" and config.get("BT") != "y"
    if no_ble:
        require(config.get("ZMK_USB") == "y",
                "a build with BLE compiled out must have CONFIG_ZMK_USB=y, or "
                "it has no transport at all")

    if config.get("SETTINGS") == "y" and not no_ble:
        for symbol in ("SETTINGS", "BT_SETTINGS", "SETTINGS_NVS"):
            require(
                config.get(symbol) == "y",
                f"a settings build needs {symbol}=y, got {config.get(symbol)!r}",
            )

    if stage == 6 and config.get("SETTINGS") != "y":
        # The settings layer stays out on purpose: this stage proves raw NVS and
        # nothing else, so a later settings failure cannot be mistaken for a
        # flash failure.
        for symbol in ("SETTINGS", "BT_SETTINGS"):
            require(
                symbol not in config,
                f"config {symbol} is enabled on stage 6; that belongs to the "
                f"next increment, not this one",
            )
    if has_storage:
        for symbol in ("FLASH", "FLASH_MAP", "NVS"):
            require(
                config.get(symbol) == "y",
                f"a storage build needs {symbol}=y, got {config.get(symbol)!r}",
            )

    for symbol, expected in REQUIRED_CONFIGS.items():
        if no_ble and symbol in ("ZMK_BLE", "BT_PERIPHERAL"):
            continue
        require(
            config.get(symbol) == expected,
            f"config {symbol} is {config.get(symbol)!r}, expected {expected!r}",
        )

    # Capture mode runs the read path and deliberately ingests nothing. A build
    # that ships with it on verifies clean, boots clean, feeds the watchdog and
    # never types - the same silent shape as an unpatched USB tree, so it gets
    # the same treatment.
    require(
        config.get("APEX_G4B_KBD_CAPTURE") != "y"
        or config.get("APEX_G4B_KSCAN_INGEST") != "y",
        "CONFIG_APEX_G4B_KBD_CAPTURE is on together with kscan ingest; capture "
        "mode never ingests a key, so this build cannot type",
    )

    # The shim is the whole reason underglow can exist without CONFIG_SPI. If
    # underglow is on, the provider must be too, or the forbidden-config list
    # above is proving the absence of something nothing was asking for.
    if config.get("ZMK_RGB_UNDERGLOW") == "y":
        require(
            config.get("LED_STRIP") == "y",
            "CONFIG_ZMK_RGB_UNDERGLOW=y without CONFIG_LED_STRIP=y; the "
            "led_strip shim is what underglow talks to",
        )

    stage = int(config.get("APEX_G4B_STAGE", "-1"))
    ingest = config.get("APEX_G4B_KSCAN_INGEST") == "y"
    studio = config.get("ZMK_STUDIO") == "y"
    require(
        not studio or config.get("SETTINGS") == "y",
        "ZMK Studio is enabled without CONFIG_SETTINGS; ZMK_STUDIO_RPC "
        "hard-selects it and the build cannot work without it",
    )
    require(
        not ingest or stage == 3,
        f"kscan ingest is enabled on stage {stage}; only stage 3 feeds ZMK",
    )
    persistent = config.get("SETTINGS") == "y" and stage != 7
    expected_name = f'"{ble_name_for(stage, ingest, studio, persistent)}"'
    require(
        no_ble or config.get("BT_DEVICE_NAME") == expected_name,
        f"config BT_DEVICE_NAME is {config.get('BT_DEVICE_NAME')!r}, expected "
        f"{expected_name!r} for stage {stage}",
    )


def check_devicetree() -> None:
    dts = ZEPHYR_DTS.read_text(encoding="utf-8", errors="replace")

    cfg = read_config()
    stage = int(cfg.get("APEX_G4B_STAGE", "-1"))
    has_storage = cfg.get("NVS") == "y" or cfg.get("SETTINGS") == "y"
    if has_storage:
        # Being allowed to write flash is only safe because where it may write
        # is pinned. Anywhere inside 0x1C000-0x67000 breaks the vendor image
        # CRC-32 and provokes the slot repair that killed the G4A cookie.
        block = re.search(r"partition@7a000\s*\{(.*?)\n\t*\};", dts, re.S)
        require(block is not None, "stage 6 has no storage partition at 0x7a000")
        if block:
            require(
                f"reg = < 0x{STORAGE_OFFSET:x} 0x{STORAGE_SIZE:x} >" in block.group(1),
                f"storage partition is not 0x{STORAGE_OFFSET:x} + "
                f"0x{STORAGE_SIZE:x}",
            )
        require(
            "partition@61000" not in dts,
            "the board's original storage partition at 0x61000 is still present; "
            "it is inside the CRC-covered vendor image",
        )
        # Belt and braces against a future edit moving it somewhere unsafe.
        for match in re.finditer(r"partition@([0-9a-f]+)\s*\{(.*?)\n\t*\};", dts, re.S):
            if 'label = "storage"' not in match.group(2):
                continue
            offset = int(match.group(1), 16)
            require(
                offset >= IMAGE_END,
                f"a storage partition at 0x{offset:x} is inside the "
                f"CRC-covered vendor image (ends 0x{IMAGE_END:x})",
            )
    else:
        require("storage_partition" not in dts, "generated DTS still defines a storage partition")
        require('label = "storage"' not in dts, "generated DTS still defines a storage label")

    # Derived, not hardcoded: the wrapper sits immediately above the code
    # partition, so a build that moves one without the other fails here rather
    # than shipping an image whose reset vector points at padding.
    dt_load_size, dt_failsafe, dt_cookie = layout()
    code = re.search(r"partition@1c000\s*\{(.*?)\n\t*\};", dts, re.S)
    require(code is not None, "generated DTS has no code partition at 0x1c000")
    if code:
        require(
            f"reg = < 0x1c000 0x{dt_load_size:x} >" in code.group(1),
            f"code partition is not 0x1c000 + 0x{dt_load_size:x} "
            f"(must end at 0x{dt_failsafe:x}, where the wrapper is)",
        )

    for name, addr in (("rr7-wrapper", f"{dt_failsafe:x}"),
                       ("g4a-cookie", f"{dt_cookie:x}")):
        require(
            f"partition@{addr}" in dts,
            f"generated DTS does not reserve the {name} page at 0x{addr}",
        )

    for node in ("spi@4002f000", "usbd@40027000"):
        block = re.search(re.escape(node) + r"\s*\{(.*?)\n\t\};", dts, re.S)
        require(block is not None, f"generated DTS has no {node} node")
        if block:
            require(
                'status = "disabled"' in block.group(1),
                f"generated DTS leaves {node} enabled",
            )

    # Both reset behaviors must survive as explicitly disabled nodes. A disabled
    # node still carries its compatible, so the status is what matters.
    reset_nodes = re.findall(r"\n\t\t(\w+): (\w+) \{\n(.*?)\n\t\t\};", dts, re.S)
    reset_seen = 0
    for _label, _name, body in reset_nodes:
        if '"zmk,behavior-reset"' not in body:
            continue
        reset_seen += 1
        require(
            'status = "disabled"' in body,
            f"reset behavior {_name} is not disabled (pulls in sys_reboot)",
        )
    require(reset_seen == 2, f"expected 2 reset behavior nodes, found {reset_seen}")
    require(
        "steelseries,apex-stm32-kscan" not in dts,
        "generated DTS still has an enabled STM32 kscan node",
    )
    require(
        "steelseries,apex-g4b-spim-kscan" in dts,
        "generated DTS is missing the G4B kscan node",
    )
    require(
        "steelseries,apex-g4a-noio-kscan" not in dts,
        "generated DTS still has the G4A no-I/O kscan node",
    )


def check_segments() -> None:
    _load_size, failsafe_address, _cookie = layout()
    text = SEGMENTS.read_text(encoding="ascii", errors="replace")
    loads = re.findall(
        r"LOAD\s+0x\w+\s+0x(\w+)\s+0x(\w+)\s+0x(\w+)\s+0x(\w+)", text
    )
    require(bool(loads), "no LOAD segments found in the ELF")
    for virt, phys, filesz, memsz in loads:
        virt_i, phys_i = int(virt, 16), int(phys, 16)
        filesz_i, memsz_i = int(filesz, 16), int(memsz, 16)

        # Flash-resident bytes live at the physical address. A .data segment has
        # its virtual address in SRAM but is loaded from flash at phys; a .bss
        # segment occupies no flash at all (filesz 0) and its phys is in SRAM.
        if filesz_i and phys_i < 0x20000000:
            require(
                IMAGE_BASE <= phys_i and phys_i + filesz_i <= failsafe_address,
                f"LOAD phys 0x{phys_i:08x}+0x{filesz_i:x} leaves the app slot",
            )
        if virt_i >= 0x20000000:
            require(
                SRAM_LOW <= virt_i and virt_i + max(memsz_i, 1) - 1 <= SRAM_HIGH,
                f"RAM segment 0x{virt_i:08x}+0x{memsz_i:x} leaves the owned SRAM",
            )
    require("0x10001000" not in text, "ELF contains a UICR segment")


def nvmc_store_offsets(payload: bytes, text_end: int) -> list[tuple[int, int]]:
    """Every NVMC register store in the payload, as (offset, base register).

    Finds each 4-byte-aligned NVMC base literal in the text region, then each
    PC-relative LDR that loads it into a register, then decodes STR.W
    (immediate, T3: 1111 1000 1100 nnnn tttt iiii iiii iiii) against that
    register within a bounded window.

    A window rather than full dataflow: the register could in principle be
    passed elsewhere, so this is not a proof. It is enough to catch a flash
    program or erase written the ordinary way, which is what it is for.
    """
    found: list[tuple[int, int]] = []
    needle = struct.pack("<I", 0x4001E000)

    for literal in range(0, text_end - 3, 4):
        if payload[literal : literal + 4] != needle:
            continue
        literal_addr = IMAGE_BASE + literal

        for pc in range(max(0, literal - 1024), literal, 2):
            half = struct.unpack_from("<H", payload, pc)[0]
            if (half & 0xF800) != 0x4800:  # LDR (literal), T1
                continue
            if ((((IMAGE_BASE + pc) + 4) & ~3) + (half & 0xFF) * 4) != literal_addr:
                continue

            base_reg = (half >> 8) & 7
            for scan in range(pc, min(pc + 128, text_end - 3), 2):
                first, second = struct.unpack_from("<HH", payload, scan)
                if (first & 0xFFF0) != 0xF8C0:  # STR.W immediate, T3
                    continue
                if (first & 0x000F) != base_reg:
                    continue
                found.append((second & 0x0FFF, base_reg))

    return found


def text_region_end() -> int:
    """File offset where the payload's code ends and constant data begins.

    Read from the link map rather than guessed, because the split is what makes
    the peripheral-reference check meaningful: a literal-pool entry is in the
    text region, a coincidental byte pattern in a const table is not.
    """
    text = (BUILD / "zephyr_final.map").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"0x0*([0-9a-f]+)\s+__rodata_region_start", text)
    require(match is not None, "link map has no __rodata_region_start")
    if match is None:
        return 0
    return int(match.group(1), 16) - IMAGE_BASE


def require_prefix_header_unmodified() -> None:
    """The frozen prefix is what stage 2 sends to the slave. Re-derive its hash.

    The header declares APEX_BOOT_PREFIX_SHA256 over the concatenated
    tx+expect_rx bytes of all 59 frames, which is what extract_boot_prefix.py
    computes. Recomputing it from the parsed arrays rather than hashing the file
    makes the check insensitive to comments and formatting and sensitive to the
    only thing that matters: a changed byte.
    """
    header = (ROOT / "apex_boot_prefix.h").read_text(encoding="ascii", errors="replace")

    declared = re.search(r'APEX_BOOT_PREFIX_SHA256\s+"([0-9a-f]{64})"', header)
    require(declared is not None, "apex_boot_prefix.h declares no sha256")
    if declared is None:
        return

    arrays = re.findall(r"\.(tx|expect_rx)\s*=\s*\{([^}]*)\}", header)
    require(
        len(arrays) == 2 * PREFIX_FRAMES,
        f"apex_boot_prefix.h has {len(arrays)} byte arrays, expected "
        f"{2 * PREFIX_FRAMES}",
    )

    blob = bytearray()
    for _name, body in arrays:
        values = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", body)]
        require(len(values) == 64, "a prefix frame is not 64 bytes")
        blob.extend(values)

    digest = hashlib.sha256(bytes(blob)).hexdigest()
    require(
        digest == declared.group(1),
        f"frozen prefix content {digest} does not match its declared "
        f"{declared.group(1)} - the table has been edited",
    )
    require(
        digest == PREFIX_SHA256,
        f"frozen prefix {digest} is not the hash this gate was reviewed "
        f"against ({PREFIX_SHA256})",
    )


def check_stage_invariants(payload: bytes, expect_stage: int | None) -> None:
    """Assert the property that defines this stage, against the binary.

    Stage 0 is OBSERVE: zero bytes on the wire. The strongest statement we can
    make about the shipped image is that the SPIM3 peripheral base does not
    appear in it at all, so no code path can reach the peripheral regardless of
    control flow. This is checked here rather than trusted from the source,
    because the source is what a reviewer reads and the binary is what runs.

    The peripheral-count ceilings come from the G4A2 payload that PASSED on
    hardware. NVMC and WDT each appear once there (Zephyr SoC init touches NVMC
    for the instruction cache), so one occurrence is the established baseline
    rather than something G4B introduced. Any increase would be a regression.
    """
    _cfg = read_config()
    stage = int(_cfg.get("APEX_G4B_STAGE", "-1"))
    has_storage = _cfg.get("NVS") == "y" or _cfg.get("SETTINGS") == "y"
    require(stage in (0, 1, 2, 3, 4, 5, 6, 7), f"stage {stage} is not design-frozen")

    # Compare the stage requested by the build script with the value resolved by
    # Kconfig. A mismatch means the wrong configuration fragment was selected or
    # edited, so the image must not be flashed.
    if expect_stage is not None:
        require(
            stage == expect_stage,
            f"built stage {stage} is not the requested stage {expect_stage}",
        )

    def occurrences(addr: int) -> int:
        return payload.count(struct.pack("<I", addr))

    spim3 = occurrences(0x4002F000)
    if stage == 0:
        require(
            spim3 == 0,
            f"SPIM3 base 0x4002F000 appears {spim3}x in a stage-0 payload; "
            "stage 0 must be incapable of touching the peripheral",
        )
    else:
        # Stages 1 and 2 legitimately talk to SPIM3, so absence would mean the
        # link module was not linked at all and nothing could have run.
        # Stage 4 is a flash survey and never enables SPIM3, so the linker
        # garbage-collects the link module. Absence there is correct; absence
        # in 1/2/3 would mean the stage could not have talked to the STM32.
        if stage in (4, 5) or (stage == 6 and not has_storage):
            require(
                spim3 == 0,
                f"SPIM3 base appears {spim3}x in a stage-{stage} payload; a "
                "read-only stage must not be able to reach the STM32 link",
            )
        else:
            require(
                spim3 > 0,
                f"SPIM3 base is absent from a stage-{stage} payload; the link "
                "module cannot have been linked in",
            )

    # The frozen configuration prefix is the thing that makes stage 2 write to
    # the slave, so its presence is the property that defines the stage. Frame
    # 0's expected response is the ASCII version string, which is a distinctive
    # marker and is checked in both directions: a stage-0 or stage-1 image that
    # contained the table would be capable of a replay it is not supposed to be
    # able to perform.
    oracle = b"3.24.1"
    if stage in (2, 3):
        require(
            oracle in payload,
            f"the frozen boot prefix is absent from a stage-{stage} payload; "
            "there is nothing to replay",
        )
        require_prefix_header_unmodified()
    else:
        require(
            oracle not in payload,
            f"a stage-{stage} payload carries the frozen boot prefix; that "
            "stage must be incapable of replaying it",
        )

    # Count aligned peripheral-base literals in .text. Matching bytes in data or
    # at unaligned offsets are not loadable literal-pool references.
    text_end = min(text_region_end(), len(payload))

    def literal_references(addr: int) -> int:
        needle = struct.pack("<I", addr)
        return sum(
            1
            for off in range(0, text_end - 3, 4)
            if payload[off : off + 4] == needle
        )

    # Stage 6 links soc_flash_nrf and therefore references NVMC for real. The
    # ceiling is raised rather than dropped, so an unexpected increase still
    # fails; the safety argument is check_devicetree() pinning the partition.
    nvmc_ceiling = 8 if has_storage else 1
    # Watchdog builds read CRV/RREN/CONFIG/RUNSTATUS and production also writes
    # the start task. Keep a ceiling so an unexpected new access still fails.
    cfg = read_config()
    feeds_wdt = (cfg.get("APEX_G4B_WATCHDOG") == "y" or
                 cfg.get("APEX_G4B_FEED_BUDGET_MS") not in (None, "0"))
    wdt_ceiling = 4 if feeds_wdt else 0
    for name, addr, ceiling in (
        ("NVMC", 0x4001E000, nvmc_ceiling),
        ("WDT", 0x40010000, wdt_ceiling),
        ("POWER", 0x40000000, 0),
    ):
        count = literal_references(addr)
        require(
            count <= ceiling,
            f"{name} base appears as {count} aligned literal(s) in the text "
            f"region, above the baseline of {ceiling}",
        )

    # No payload of any stage may program or erase flash.
    #
    # "NVMC base absent" was tried first and is not achievable: Zephyr's nRF SoC
    # init enables the instruction cache through NVMC->ICACHECNF, so exactly one
    # reference is always present and always legitimate. Asserting zero asserts
    # something the platform makes impossible.
    #
    # What IS checkable, and is the actual safety property: every NVMC store in
    # the payload targets ICACHECNF at +0x540, and none targets CONFIG (+0x504),
    # ERASEPAGE (+0x508), ERASEALL (+0x50C) or ERASEUICR (+0x514). The wrapper
    # does use NVMC to erase the first application page - that is the recovery
    # path and is checked separately against the wrapper disassembly.
    # Stage 6 programs flash through soc_flash_nrf, so NVMC stores beyond
    # ICACHECNF are expected there and the placement checks carry the safety
    # argument instead.
    for offset, register in (
        [] if has_storage else nvmc_store_offsets(payload, text_end)
    ):
        require(
            offset == 0x540,
            f"payload stores to NVMC+0x{offset:03X} (r{register}); only "
            f"ICACHECNF at +0x540 is permitted - the payload must never "
            f"program or erase flash",
        )

    # The retained one-shot register specifically. The payload has no business
    # touching it: arming and disarming belong to the wrapper alone, and a
    # payload that wrote it could disarm the recovery path from inside the very
    # image that path exists to recover from.
    require(
        literal_references(GPREGRET2) == 0,
        "the payload references GPREGRET2; only the wrapper may touch it",
    )


def check_symbols() -> None:
    # nm emits every CONFIG_* Kconfig value as an absolute ('A') symbol, so
    # CONFIG_HAS_HW_NRF_NVMC_PE and CONFIG_USB_DEVICE_VID would otherwise read
    # as linked drivers. Only real code/data symbols are meaningful here.
    names = []
    for line in SYMBOLS.read_text(encoding="ascii", errors="replace").splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3 or parts[1].upper() == "A":
            continue
        names.append(parts[2].lower())
    linked = "\n".join(names)

    cfg = read_config()
    has_storage = cfg.get("NVS") == "y" or cfg.get("SETTINGS") == "y"
    has_usb = cfg.get("ZMK_USB") == "y"
    for needle in FORBIDDEN_SYMBOLS + ("sys_reboot",):
        # Stage 6 links the flash stack on purpose. The exemption is narrow and
        # is paid for by check_devicetree() pinning WHERE it may write.
        if has_storage and needle in STAGE7_ALLOWED_SYMBOLS:
            continue
        if has_usb and needle in USB_ALLOWED_SYMBOLS:
            continue
        hits = [name for name in names if needle in name]
        require(not hits, f"payload links forbidden symbol(s) {hits} matching {needle!r}")
    require(bool(linked), "symbol table is empty")


def check_zephyr_patches() -> None:
    """Fail if a USB build is made against an unpatched Zephyr tree.

    The vendored Zephyr tree is not under version control here, so a re-fetch
    silently reverts local changes. Without the USBPWRRDY patch the image builds,
    verifies and runs perfectly while USB never enumerates at all - no COM port,
    no HID device, nothing - which is a slow and unpleasant thing to re-diagnose.
    See ZEPHYR_PATCHES.md.
    """
    if read_config().get("ZMK_USB") != "y":
        return

    src = (ZEPHYR_BASE / "drivers" / "usb" /
           "device" / "usb_dc_nrfx.c")
    require(src.is_file(), f"cannot find {src} to check for the USBPWRRDY patch")
    text = src.read_text(encoding="utf-8", errors="replace")

    # Match on the behaviour, not on a comment marker: a reworded patch is fine,
    # a deleted one is not.
    require("NRFX_POWER_USB_STATE_READY" in text
            and "NRFX_POWER_USB_EVT_READY" in text
            and text.index("NRFX_POWER_USB_STATE_READY")
            > text.index("nrfx_power_usbevt_enable"),
            "usb_dc_nrfx.c does not synthesise NRFX_POWER_USB_EVT_READY in "
            "usb_dc_attach(); USB will not enumerate. See ZEPHYR_PATCHES.md")

    # Patch #2: the pull-up must not be asserted before the USB supply is up.
    common = (ZEPHYR_BASE / "drivers" / "usb" /
              "common" / "nrf_usbd_common" / "nrf_usbd_common.c")
    require(common.is_file(), f"cannot find {common} to check the OUTPUTRDY gate")
    ctext = common.read_text(encoding="utf-8", errors="replace")
    pull = ctext.find("NRF_USBD->USBPULLUP = 1")
    gate = ctext.find("POWER_USBREGSTATUS_OUTPUTRDY_Msk")
    require(gate != -1 and pull != -1 and gate < pull,
            "nrf_usbd_common_start() asserts USBPULLUP without first waiting "
            "for USBREGSTATUS.OUTPUTRDY; the pull-up has no supply and D+ never "
            "rises. See ZEPHYR_PATCHES.md")


# GPIO register names. pins_g4b.h has always stated that only pins_g4b.c may
# touch these and that this file enforces it. It did not: a grep for any of
# these in verify_g4b.py returned zero. The claim outlived the check, and a
# direct GPIO write added to link_g4b.c during the USB investigation went
# unnoticed for exactly that reason.
GPIO_REGISTER_TOKENS = ("OUTSET", "OUTCLR", "DIRSET", "DIRCLR", "PIN_CNF")
GPIO_OWNER = "pins_g4b.c"


def strip_comments(path: Path) -> str:
    """Blank out comments, preserving line numbering.

    Needed because this file's own explanatory comments name the very registers
    it is scanning for - the first version of the check reported them as
    violations, which is a good illustration of why it strips rather than
    substring-matches.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def check_watchdog_feed(payload: bytes) -> None:
    """The watchdog feed is allowed, but only in the exact shape that keeps
    recovery guaranteed.

    This replaces a blanket "the reload magic must not appear" rule. These
    assertions keep the feed site auditable:

      - the magic appears at most ONCE, so there is a single feed site to reason
        about rather than a feed scattered through the payload;
      - if it appears, the RR[7] address literal must appear too. The wrapper
        arms RREN=0x80, so RR[7] at 0x4001061C is the live channel; a feed
        written to RR[0] compiles, links, runs, and silently does nothing;
      - legacy diagnostic builds must retain their fixed feed budget;
      - production builds use the health-gated watchdog and A/B recovery, not
        the legacy "feed unbounded" switch.
    """
    occurrences = payload.count(WDT_RELOAD_MAGIC)
    require(occurrences <= 1,
            f"watchdog reload magic appears {occurrences} times; there must be "
            f"exactly one feed site")

    cfg = read_config()
    budget = cfg.get("APEX_G4B_FEED_BUDGET_MS")
    production_watchdog = cfg.get("APEX_G4B_WATCHDOG") == "y"

    if occurrences == 0:
        require(budget is None or budget == "0",
                "a feed budget is configured but the payload contains no reload "
                "magic - the feed is not actually compiled in")
        return

    # The RR[7] channel is pinned by a BUILD_ASSERT in src/wdt_g4b.c, not here:
    # the compiler materialises 0x4001061C with movw/movt and never emits it as
    # a word, so searching the image for it fails on a correct build.
    if production_watchdog:
        require(cfg.get("APEX_G4B_FEED_UNBOUNDED") != "y",
                "the production watchdog must not use the legacy unbounded "
                "diagnostic feed option")
    else:
        require(budget is not None and budget.isdigit() and int(budget) > 0,
                f"the diagnostic payload feeds the watchdog with no positive "
                f"CONFIG_APEX_G4B_FEED_BUDGET_MS (got {budget!r})")

    # The pin beacon holds candidate pins as push-pull outputs indefinitely so a
    # multimeter can identify pads. That is a measurement build's job and never
    # a keyboard's - and three pins the electrical survey called free turned out
    # to be the SPI-NOR MISO, the recovery UART RX and a boot strap, so driving
    # the wrong one is contention against something real.
    require(
        cfg.get("APEX_G4B_PIN_BEACON") != "y"
        or cfg.get("APEX_G4B_KSCAN_INGEST") != "y",
        "CONFIG_APEX_G4B_PIN_BEACON is on in a keyboard build; it drives free "
        "pins as outputs indefinitely and is diagnostic only",
    )


def check_gpio_ownership() -> None:
    """Only pins_g4b.c may name a GPIO output register.

    Keeping every pin write in one file is what makes the "never drive a line
    the STM32 owns" rule auditable at all - the enum in pins_g4b.h cannot even
    express P0.11/19/23, but that only helps if nothing else writes the
    registers directly.
    """
    src = ROOT / "src"
    if not src.is_dir():
        return
    offenders = []
    for path in sorted(src.glob("*.c")) + sorted(src.glob("*.h")):
        if path.name in (GPIO_OWNER, "pins_g4b.h"):
            continue
        for lineno, line in enumerate(strip_comments(path).splitlines(), 1):
            if any(tok in line for tok in GPIO_REGISTER_TOKENS):
                offenders.append(f"{path.name}:{lineno}: {line.strip()[:70]}")
    require(
        not offenders,
        "GPIO registers are written outside " + GPIO_OWNER + ": "
        + "; ".join(offenders[:4]),
    )


def check_wrapper(payload_reset: int, payload_sp: int) -> None:
    disasm = FAILSAFE_DISASM.read_text(encoding="ascii", errors="replace").lower()

    require("40010000" in disasm and "#1288" in disasm, "wrapper lacks WDT-base/RREN access")
    expected_wdt_writes = (
        r"mov\.w\s+r1, #1966080.*\n.*str\.w\s+r1, \[r0, #1284\]",
        r"mov\.w\s+r1, #128.*\n.*str\.w\s+r1, \[r0, #1288\]",
        r"mov\.w\s+r1, #9.*\n.*str\.w\s+r1, \[r0, #1292\]",
    )
    for pattern in expected_wdt_writes:
        require(
            re.search(pattern, disasm) is not None,
            f"wrapper WDT constant/write mismatch: {pattern}",
        )

    # The one-shot must live in the retained register, never in flash. G4A's
    # flash cookie sat inside the CRC-covered region and never latched.
    require(f"{GPREGRET2:08x}" in disasm, "wrapper does not reference GPREGRET2")

    # Three distinct behaviours must all be present. Checking only that the
    # magic constant appears is not enough: a wrapper that compares against the
    # magic but never stores it would pass while reproducing the G4A failure
    # exactly, because the one-shot would never arm.
    magic = RETAINED_COOKIE_MAGIC
    require(
        re.search(rf"cmp\s+r\d+, #{magic}\b", disasm) is not None,
        "wrapper does not test the retained cookie magic",
    )
    # objdump appends a "; 0x47" comment after the immediate, so the pair must
    # be matched line-wise rather than with a bare \s*\n between them.
    require(
        re.search(
            rf"movs\s+r(\d+), #{magic}\b[^\n]*\n[^\n]*\bstr\s+r\1, \[r\d+", disasm
        )
        is not None,
        "wrapper does not store the retained cookie magic (one-shot would never arm)",
    )
    # The disarm must happen on the recover path BEFORE the erase, so a fresh
    # image does not inherit an armed cookie and fail closed on its first launch.
    #
    # Bound this by the erase itself rather than by a line count. The original
    # form allowed four lines after <recover>: and rejected a build whose only
    # change was an added diagnostic beacon ahead of the disarm - a correct
    # image failing a positional assertion. "Before the erase" is the actual
    # requirement; "within four instructions" was a proxy for it.
    recover_body = disasm.split("<recover>:", 1)
    require(len(recover_body) == 2, "wrapper has no recover label")
    # NVMC CONFIG (offset 0x504 = 1284) is written to enable the erase. Cut at
    # the first NVMC write inside the recover path; the WDT also uses #1284,
    # but that is upstream of <recover>: and so is not in this slice.
    before_erase = re.split(r"str\.w\s+r\d+, \[r\d+, #1284\]", recover_body[1], 1)[0]
    require(
        re.search(
            r"\bmovs\s+r(\d+), #0\b[^\n]*\n[^\n]*\bstr\s+r\1, \[r\d+", before_erase
        )
        is not None,
        "wrapper recover path does not disarm the retained cookie before erasing",
    )
    require("4d000" not in disasm, "wrapper still references the retired flash cookie page")
    require(
        f"{G4A_COOKIE_MAGIC:08x}" not in disasm,
        "wrapper still carries the retired G4A flash cookie magic",
    )
    require(
        f"{BEACON_COOKIE_MAGIC:08x}" not in disasm,
        "wrapper still carries the RR7 beacon cookie magic",
    )
    require(
        re.search(rf"cmp\s+r\d+, #{G4A2_RETAINED_MAGIC}\b", disasm) is None,
        "wrapper still tests the G4A2 retained magic - it was not rebuilt for G4B",
    )
    require("1c000" in disasm, "wrapper lacks the first-page erase target")
    require(f"{payload_reset:08x}" in disasm, "wrapper does not carry the fresh reset vector")
    require(f"{payload_sp:08x}" in disasm, "wrapper does not carry the fresh stack pointer")
    require("0001d001" not in disasm, "wrapper still carries a stale RR7 beacon reset vector")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--expect-stage",
        type=int,
        choices=(0, 1, 2, 3, 4, 5, 6, 7),
        default=None,
        help="assert the built stage matches the stage the build script asked for",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / "build",
        help="CMake build directory containing zephyr/ (default: repo-local build)",
    )
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        default=ROOT / "artifacts",
        help="packaged artifact directory (default: repo-local artifacts)",
    )
    parser.add_argument(
        "--zephyr-base",
        type=Path,
        default=ROOT.parent / "zmk-upstream" / "zephyr",
        help="Zephyr source root used for required patch checks",
    )
    args = parser.parse_args()
    configure_paths(args.build_dir, args.artifact_dir, args.zephyr_base)

    image = IMAGE.read_bytes()
    payload = PAYLOAD.read_bytes()
    failsafe = FAILSAFE.read_bytes()
    payload_sp, payload_reset = struct.unpack_from("<II", payload, 0)

    require(len(image) == IMAGE_SIZE, f"vendor image size is {len(image)}")
    require(zlib.crc32(image) & 0xFFFFFFFF == CRC32_RESIDUE, "vendor CRC residue")
    stored_crc = struct.unpack_from("<I", image, TRAILER_OFFSET)[0]
    require(
        stored_crc == zlib.crc32(image[:TRAILER_OFFSET]) & 0xFFFFFFFF,
        "stored trailer CRC does not match the image body",
    )
    load_size, failsafe_address, cookie_address = layout()
    require(
        struct.unpack_from("<II", image, 0)
        == (0x20020000, failsafe_address | 1),
        f"vendor-facing vectors: reset must be 0x{failsafe_address | 1:08x} for "
        f"a wrapper at 0x{failsafe_address:05x}",
    )
    require(
        failsafe_address + 0x1000 <= IMAGE_BASE + IMAGE_SIZE,
        f"wrapper at 0x{failsafe_address:05x} does not leave a page inside the image",
    )

    failsafe_offset = failsafe_address - IMAGE_BASE
    cookie_offset = cookie_address - IMAGE_BASE
    require(
        image[failsafe_offset : failsafe_offset + len(failsafe)] == failsafe,
        "wrapper placement at image offset 0x30000",
    )
    # The retired cookie page must now be ordinary zero padding, not an armed or
    # erased word, so nothing in the image can be mistaken for a one-shot state.
    require(
        image[cookie_offset : cookie_offset + 4] == b"\x00" * 4,
        "retired cookie page is not plain zero padding",
    )
    require(image[8 : len(payload)] == payload[8:], "payload placement at app offset 0")
    require(
        IMAGE_BASE + len(payload) <= failsafe_address,
        f"payload ends at 0x{IMAGE_BASE + len(payload):08x}, at or past the wrapper",
    )
    require(
        image[len(payload) : failsafe_offset] == b"\x00" * (failsafe_offset - len(payload)),
        "gap between payload and wrapper is not zero-filled",
    )
    check_watchdog_feed(payload)
    require(payload_reset & 1 == 1, "payload reset vector is not an odd Thumb address")

    check_config()
    check_gpio_ownership()
    check_zephyr_patches()
    check_stage_invariants(payload, args.expect_stage)
    check_devicetree()
    check_segments()
    check_symbols()
    check_wrapper(payload_reset, payload_sp)

    if failures:
        print("verification=FAIL")
        for failure in failures:
            print(f"  reason={failure}")
        return 1

    print("verification=PASS")
    print(f"image={IMAGE}")
    print(f"size={len(image)}")
    print(f"sha256={hashlib.sha256(image).hexdigest()}")
    print(f"trailer_crc32=0x{stored_crc:08x}")
    print(f"payload_size={len(payload)}")
    print(f"payload_end_address=0x{IMAGE_BASE + len(payload):08x}")
    print(f"payload_sp=0x{payload_sp:08x}")
    print(f"payload_reset=0x{payload_reset:08x}")
    print(f"payload_sha256={hashlib.sha256(payload).hexdigest()}")
    print(f"failsafe_sha256={hashlib.sha256(failsafe).hexdigest()}")
    _cfg = read_config()
    _stage = int(_cfg.get('APEX_G4B_STAGE', '-1'))
    print(f"ble_name={ble_name_for(_stage, _cfg.get('APEX_G4B_KSCAN_INGEST') == 'y', _cfg.get('ZMK_STUDIO') == 'y', _cfg.get('SETTINGS') == 'y' and _stage != 7)}")
    print(f"kscan_ingest={_cfg.get('APEX_G4B_KSCAN_INGEST', 'n')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
