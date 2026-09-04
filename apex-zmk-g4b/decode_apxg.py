#!/usr/bin/env python3
"""Decode APXG evidence records from a G4B UART capture.

Stage 0 produces no BLE-visible result, so these records are its only output.
The same capture also carries the recovery wrapper's 10-byte APXB boot beacons
("APXB" + RESETREAS + CRLF), which are decoded here too because they say how
many times the payload launched and why it reset.

Layout must match struct g4b_record in src/evidence_g4b.h exactly.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

APXG_MAGIC = b"APXG"
APXB_MAGIC = b"APXB"

# The layout mirrors struct g4b_record in evidence_g4b.h.
LAYOUT = "<IHH" + "II" + "IIII" + "IIII" + "BBBBBBBB" + "IIII" + "IIII"
FIELDS = [
    "magic", "version", "stage",
    "launch_ms", "last_error",
    "p0_in_before", "p1_in_before", "p0_dir_before", "p1_dir_before",
    "p0_in_after", "p1_in_after", "p0_dir_after", "p1_dir_after",
    "ready_pullup_before", "ready_pulldown_before",
    "attn_pullup_before", "attn_pulldown_before",
    "ready_pullup_after", "ready_pulldown_after",
    "attn_pullup_after", "attn_pulldown_after",
    "dwt_enable_to_ready", "dwt_observe_window", "ready_edges", "bringup_ok",
    "gpiote_config0", "pin_cnf_ready", "pin_cnf_attn", "pin_cnf_miso",
]
RECORD_SIZE = struct.calcsize(LAYOUT)

# Stage 1, version 2: struct g4b_record_s1 in evidence_g4b.h
S1_HEAD = "<IHH IIII BBBB".replace(" ", "")
S1_EXCH = "<64s IIIIII".replace(" ", "")
S1_SIZE = struct.calcsize(S1_HEAD) + 2 * struct.calcsize(S1_EXCH)

SPIM_RESULT = {
    0: "OK", 1: "NOT_READY_TX", 2: "NOT_READY_RX",
    3: "TX_TIMEOUT", 4: "RX_TIMEOUT",
}

# Stage 2: version 3 is a REPLAY launch, version 4 a CONFIRM launch.
# struct g4b_s2_head, then the per-version body.
S2_HEAD = "<IHH IIII II".replace(" ", "")
S2_HEAD_SIZE = struct.calcsize(S2_HEAD)
PREFIX_FRAMES = 59
VERBATIM_INDEX = (0, 29, 57, 58)
S2_REPLAY = f"<IIII {PREFIX_FRAMES}s B 64s 64s {4 * 64}s".replace(" ", "")
S2_REPLAY_SIZE = S2_HEAD_SIZE + struct.calcsize(S2_REPLAY)
# Version 5 appends the post-replay 0xA0 probe. Version 3 is kept parseable
# because the 2026-08-04 G4B-2 capture is evidence.
S2_REPLAY2_SIZE = S2_REPLAY_SIZE + struct.calcsize(S1_EXCH)
S2_CONFIRM = "<BBBB".replace(" ", "")
S2_CONFIRM_SIZE = S2_HEAD_SIZE + struct.calcsize(S2_CONFIRM) + 2 * struct.calcsize(S1_EXCH)

# Stage 3, version 6. head + replay summary + post-replay A0 + poll stats + events
S3_SUMMARY = "<IIII".replace(" ", "")
S3_STATS = "<IIIIII".replace(" ", "")
S3_EVENT = "<II32s".replace(" ", "")
S3_EVENTS = 8
S3_SIZE = (S2_HEAD_SIZE + struct.calcsize(S3_SUMMARY) + struct.calcsize(S1_EXCH)
           + struct.calcsize(S3_STATS) + S3_EVENTS * struct.calcsize(S3_EVENT))
# Version 11 appends the ingest diagnostics. Version 6 stays parseable.
S3_DIAG = "<iIIIII"
S3_DIAG_SIZE = S3_SIZE + struct.calcsize(S3_DIAG)
# Version 12 appends the USB state. Version 11 stays parseable.
S3_USB = "<III"
S3_USB_SIZE = S3_DIAG_SIZE + struct.calcsize(S3_USB)

# Version 13 appends the USBD peripheral registers.
S3_USBD = "<IIII"
S3_USBD_SIZE = S3_USB_SIZE + struct.calcsize(S3_USBD)

# Version 14 appends the USBPWRRDY re-post outcome.
S3_KICK = "<I"
S3_KICK_SIZE = S3_USBD_SIZE + struct.calcsize(S3_KICK)
USB_KICK = {
    0: "APPLIED - re-posted the missing USBPWRRDY event",
    1: "not needed - the driver had already attached",
    2: "USBD was never enabled - a different fault",
    3: "regulator not ready - READY was legitimately still pending",
    4: "USBPWRRDY interrupt not armed - re-posting would have been inert",
}

# Version 15 appends the interrupt-delivery evidence around the re-post.
S3_IRQ = "<IIIII"
S3_IRQ_SIZE = S3_KICK_SIZE + struct.calcsize(S3_IRQ)

# Version 16 appends usb_enable()'s own verdict.
S3_EN = "<iII"
S3_EN_SIZE = S3_IRQ_SIZE + struct.calcsize(S3_EN)

# Version 17 appends USBD->ENABLE as found before the USB stack ran.
S3_PRE = "<I"
S3_PRE_SIZE = S3_EN_SIZE + struct.calcsize(S3_PRE)

# Version 18 appends the forced-pullup probe.
S3_FORCE = "<IIII"
S3_FORCE_SIZE = S3_PRE_SIZE + struct.calcsize(S3_FORCE)

# Version 19 appends the force-normal probe.
S3_NORM = "<IIII"
S3_NORM_SIZE = S3_FORCE_SIZE + struct.calcsize(S3_NORM)

# Version 20 appends three 12-word USBD/CLOCK snapshots.
S3_SNAP_WORDS = 12
S3_SNAPS = 4
S3_SNAP = f"<{S3_SNAP_WORDS * S3_SNAPS}I"
S3_SNAP_SIZE = S3_NORM_SIZE + struct.calcsize(S3_SNAP)
# Version 21 widens the snapshot to four and appends the re-enable result.
S3_RE = "<III"
S3_RE_SIZE = S3_SNAP_SIZE + struct.calcsize(S3_RE)
# Version 22 appends the errata gate and the forced-workaround retry.
S3_ERR = "<IIIIIII"
S3_ERR_SIZE = S3_RE_SIZE + struct.calcsize(S3_ERR)
# Version 23 appends the early-window sampler.
S3_EARLY = "<IIIIIII"
S3_EARLY_SIZE = S3_ERR_SIZE + struct.calcsize(S3_EARLY)
# Version 24 appends the deliberate re-attach.
S3_RE2 = "<IIIII"
S3_RE2_SIZE = S3_EARLY_SIZE + struct.calcsize(S3_RE2)
# Version 25 appends the driver-level re-attach.
S3_RE3 = "<iiII"
S3_RE3_SIZE = S3_RE2_SIZE + struct.calcsize(S3_RE3)
# Version 26 appends the pre-USB power/clock snapshots.
S3_PWR_WORDS = 7
S3_PWR = f"<{S3_PWR_WORDS * 2}I"
S3_PWR_SIZE = S3_RE3_SIZE + struct.calcsize(S3_PWR)
# Version 27 appends the USB-regulator wait.
S3_RW = "<III"
S3_RW_SIZE = S3_PWR_SIZE + struct.calcsize(S3_RW)
# Version 28 appends the per-write trigger hunt.
S3_TRIG = "<IIIII"
S3_TRIG_SIZE = S3_RW_SIZE + struct.calcsize(S3_TRIG)
# Version 29 appends the D+ hold result.
S3_HOLD = "<III"
S3_HOLD_SIZE = S3_TRIG_SIZE + struct.calcsize(S3_HOLD)
# Version 30 appends the P0.26 experiment. Those three words have TWO meanings,
# split by version exactly as the kbd_cap_* block is: in a version 32..37 record
# they are the P0.26 experiment (real data only in the archived h7-p026 and
# h8-loadergpio captures; zero everywhere since, because the experiment was
# removed), and in a version-39 record they are the gamepad endpoint counters
# gp_writes / gp_busy / gp_err. Same offsets, same 1280-byte record.
S3_P26 = "<III"
S3_P26_SIZE = S3_HOLD_SIZE + struct.calcsize(S3_P26)
# Version 31 appends the escape-hatch chord window.
S3_CHORD = "<IIII9sBH"
S3_CHORD_SIZE = S3_P26_SIZE + struct.calcsize(S3_CHORD)
# Must match G4B_SURVEY_P0_MASK / G4B_SURVEY_P1_MASK in src/link_g4b.c.
SURVEY_P0_MASK = 0xF073F302
SURVEY_P1_MASK = 0x000000DE

MODE_NAME = {0: "BT", 1: "USB", 2: "DONGLE"}
# Must match G4B_MODE_USB_MIN_MV / G4B_MODE_DONGLE_MIN_MV in src/mode_g4b.c.
MODE_USB_MIN_MV = 900
MODE_DONGLE_MIN_MV = 2600
# src/mode_g4b.h
MODE_SEEN_HIGH = 0x10
MODE_SEEN_LOW = 0x20
MODE_CLASS_PRESENT = 0x80
MODE_CLASS_MASK = 0x0F
# Version 32 appends continuous 0xA0 travel monitoring.
S3_TRAVEL = "<IIIIII"
S3_TRAVEL_SIZE = S3_CHORD_SIZE + struct.calcsize(S3_TRAVEL)
# Version 33 appends the time-bounded watchdog feed telemetry.
S3_WDT = "<IIIIII"
S3_WDT_SIZE = S3_TRAVEL_SIZE + struct.calcsize(S3_WDT)
# Version 34 appends the uptime at which the USB watch began.
S3_START = "<I"
S3_START_SIZE = S3_WDT_SIZE + struct.calcsize(S3_START)

# Version 35 appends the mode-switch ADC sweep: min, max and last raw counts
# for the five analog pins that are not part of the STM32 link, plus a sample
# count. Order matches g4b_saadc_pselp.
S3_AIN = "<5H5H5HH"
S3_AIN_SIZE = S3_START_SIZE + struct.calcsize(S3_AIN)

# Version 36 appends SPIM2 as the vendor loader left it, to recover the
# IS31FL3743B SCK/MOSI pins that the stock image does not contain.
S3_SPIM2 = "<8I"
S3_SPIM2_SIZE = S3_AIN_SIZE + struct.calcsize(S3_SPIM2)

# Version 37 appends the keyboard-loop read capture: reads, ok, a 12-bucket
# popcount histogram, max popcount, sample count, and 8 sample reads (9 bitmap
# bytes + status + SPIM result each).
S3_KBDCAP = "<II14H88s"
S3_KBDCAP_SIZE = S3_SPIM2_SIZE + struct.calcsize(S3_KBDCAP)
AIN_PINS = ("P0.03", "P0.28", "P0.29", "P0.30", "P0.31")
WDT_STOP = {0: "still feeding", 1: "budget reached - feeding stopped",
            3: "DONGLE position - feeding withheld, heading for recovery",
            4: "ESCAPE HATCH STALE - refused to feed, heading for recovery",
            2: "watchdog config not ours - refused to feed",
            7: "keyboard scan loop stopped"}
TRIG_WHICH = {0: "none - our writes do NOT start it either",
              1: "the errata 187/211 trim window",
              2: "cycling USBD->ENABLE",
              3: "USBD->LOWPOWER = 0",
              4: "re-posting EVENTS_USBPWRRDY"}
PWR_NAMES = ["RESETREAS", "HFCLKSTAT", "LFCLKSTAT", "LFCLKSRC",
             "DCDCEN", "DCDCEN0", "USBREGSTATUS"]
PWR_WHEN = ["PRE_KERNEL_1 (inherited)", "after usb_enable()"]
SNAP_NAMES = ["ENABLE", "USBPULLUP", "INTEN", "EVENTCAUSE", "LOWPOWER",
              "EPINEN", "EPOUTEN", "FRAMECNTR", "DPDMVALUE", "USBREGSTATUS",
              "HFCLKSTAT", "HFCLKRUN"]
SNAP_WHEN = ["stage entry", "after HFXO+normal", "after forcing pullup",
             "after ENABLE cycle"]

# enum usb_dc_status_code, Zephyr: ERROR=0 RESET=1 CONNECTED=2 CONFIGURED=3
# DISCONNECTED=4 SUSPEND=5 RESUME=6 INTERFACE=7 SET_HALT=8 CLEAR_HALT=9 SOF=10
USB_DC_STATUS = {0: "ERROR", 1: "RESET", 2: "CONNECTED", 3: "CONFIGURED",
                 4: "DISCONNECTED", 5: "SUSPEND", 6: "RESUME", 7: "INTERFACE",
                 8: "SET_HALT", 9: "CLEAR_HALT", 10: "SOF"}
USB_CONN_STATE = {0: "NONE", 1: "POWERED", 2: "HID"}
BITMAP_BYTES = 9

# The two keys the operator is asked to press. Maximally separable: different
# bytes of the bitmap, different bit positions, opposite corners of the board.
EXPECTED_KEYS = ("Q", "SLASH")

SCAN_MAP = Path(__file__).resolve().parent.parent / "apex-zmk-slot" / "scan-map.json"


def load_scan_map() -> dict[int, str]:
    """scan_bit -> key name. Absent map is not fatal; bits print numerically."""
    try:
        data = json.loads(SCAN_MAP.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {k["scan_bit"]: k["key"] for k in data.get("keys", [])}


def bits_set(bitmap: bytes) -> list[int]:
    """LSB-first within each byte, as the protocol header records."""
    return [
        byte_index * 8 + bit
        for byte_index in range(BITMAP_BYTES)
        for bit in range(8)
        if bitmap[byte_index] & (1 << bit)
    ]


# Stage 4, version 7: the read-only flash survey.
S4_BODY = "<I 2I 2I III III 128s 256s".replace(" ", "")
S4_SIZE = S2_HEAD_SIZE + struct.calcsize(S4_BODY)
S4_MAX_PAGES = 64

LAUNCH_KIND = {
    0: "REPLAY (SREQ set: first launch after the flash)",
    1: "CONFIRM (watchdog relaunch, read-only)",
    2: "CONFIRM_NO_BEACON (wrapper beacon absent - defaulted to read-only)",
}

FRAME_RESULT = {0: ".", 1: "M", 2: "X", 3: "E"}
NO_INDEX = 0xFFFFFFFF

ERRORS = {0: "none", 1: "PIN_DIR (a line we must not drive was an output)",
          2: "DEADLINE", 3: "NOT_READY (ready line never asserted)",
          4: "REPLAY (aborted on a frame mismatch or SPIM error)"}

RESETREAS_BITS = [(0x01, "RESETPIN"), (0x02, "DOG"), (0x04, "SREQ"),
                  (0x08, "LOCKUP"), (0x10000, "OFF"), (0x20000, "LPCOMP"),
                  (0x40000, "DIF"), (0x80000, "NFC"), (0x100000, "VBUS")]

CPU_HZ = 64_000_000


def pull_verdict(up: int, down: int) -> str:
    """A line that follows the pull is floating. One that ignores it is driven."""
    if up == 1 and down == 0:
        return "FLOATING (follows the pull)"
    if up == down == 1:
        return "DRIVEN HIGH (ignores pulldown)"
    if up == down == 0:
        return "DRIVEN LOW (ignores pullup)"
    return f"INCONSISTENT (up={up} down={down})"


def ascii_of(raw: bytes) -> str:
    return "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in raw)


S6_BODY = "<IIIIiiiIII32s32s"
S6_SIZE = S2_HEAD_SIZE + struct.calcsize(S6_BODY)

S7_BODY = "<iiiIIII32s32s"
S7_SIZE = S2_HEAD_SIZE + struct.calcsize(S7_BODY)


def print_s7(data: bytes, i: int) -> None:
    (_magic, _ver, stage, launch_ms, _err, _bringup,
     _e2r, resetreas, kind) = struct.unpack_from(S2_HEAD, data, i)
    (init_rc, load_rc, save_rc, found_start, found_after,
     counter_read, counter_write, before, after) = struct.unpack_from(
        S7_BODY, data, i + S2_HEAD_SIZE)

    print(f"\n--- stage-7 settings record at 0x{i:05X} ---")
    names = "|".join(n for b, n in RESETREAS_BITS if resetreas & b) or "none"
    print(f"  launch_ms={launch_ms} resetreas=0x{resetreas:08X} ({names}) "
          f"launch_kind={kind}")
    print(f"  settings_subsys_init rc={init_rc}  "
          f"settings_load_subtree rc={load_rc}  settings_save_one rc={save_rc}")
    print(f"  key found by ZMK's own settings_load(): "
          f"{'yes' if found_start else 'no'}")
    print(f"  key found after our subtree load       : "
          f"{'yes' if found_after else 'no'}")
    print(f"  counter read  = {counter_read}")
    print(f"  counter write = {counter_write}")
    print(f"\n  flash at 0x7A000 before: {before[:16].hex(' ')}")
    print(f"  flash at 0x7A000 after : {after[:16].hex(' ')}")

    print("\n  >>> VERDICT")
    if init_rc != 0:
        print(f"      FAIL - settings_subsys_init returned {init_rc}")
    elif save_rc != 0:
        print(f"      FAIL - settings_save_one returned {save_rc}")
    elif not found_after:
        print("      FIRST LAUNCH - no key stored yet; wrote the first value.")
        print("      A later launch finding it is the result that matters.")
    else:
        print(f"      PERSISTED - the settings layer read {counter_read} written by")
        print(f"      a previous launch and stored {counter_write}. This is the layer")
        print("      BLE bonds use, on the partition stage 6 proved.")
        if found_start:
            print("      ZMK's own settings_load() also found it, so the standard")
            print("      boot path restores stored settings unaided.")


def print_s6(data: bytes, i: int) -> None:
    (_magic, _ver, stage, launch_ms, last_error, _bringup,
     _e2r, resetreas, kind) = struct.unpack_from(S2_HEAD, data, i)
    (offset, size, sector_size, sector_count, mount_rc, read_rc, write_rc,
     magic_read, counter_read, counter_write,
     before, after) = struct.unpack_from(S6_BODY, data, i + S2_HEAD_SIZE)

    print(f"\n--- stage-6 NVS record at 0x{i:05X} ---")
    names = "|".join(n for b, n in RESETREAS_BITS if resetreas & b) or "none"
    print(f"  launch_ms={launch_ms} resetreas=0x{resetreas:08X} ({names}) "
          f"launch_kind={kind}")
    print(f"  partition 0x{offset:05X} + 0x{size:04X}  "
          f"sector {sector_size} x {sector_count}")
    print(f"  nvs_mount rc={mount_rc}  nvs_read rc={read_rc}  nvs_write rc={write_rc}")

    print(f"\n  flash at 0x{offset:05X} before: {before[:16].hex(' ')}")
    print(f"  flash at 0x{offset:05X} after : {after[:16].hex(' ')}")
    if before == after:
        print("    (unchanged this launch)")

    print(f"\n  counter read  = {counter_read} "
          f"(magic 0x{magic_read:08X}"
          f"{' OK' if magic_read == 0x58455041 else ' - nothing stored yet'})")
    print(f"  counter written = {counter_write}")

    print("\n  >>> VERDICT")
    if mount_rc != 0:
        print(f"      FAIL - nvs_mount returned {mount_rc}")
    elif write_rc < 0:
        print(f"      FAIL - nvs_write returned {write_rc}")
    elif magic_read != 0x58455041:
        print("      FIRST LAUNCH - nothing was stored yet, wrote the first value.")
        print("      A later launch finding it is the result that matters.")
    else:
        print(f"      PERSISTED - found {counter_read} from a previous launch and")
        print(f"      stored {counter_write}. Flash survives the watchdog reset,")
        print("      which is the property BLE bonds need.")


def print_s4(data: bytes, i: int) -> None:
    (_magic, _ver, stage, launch_ms, last_error, bringup_ok,
     _e2r, resetreas, kind) = struct.unpack_from(S2_HEAD, data, i)
    (page_size, r0s, r1s, r0e, r1e, surveyed, erased, zero,
     approtect, nrffw0, bootaddr, samples, crcs) = struct.unpack_from(
        S4_BODY, data, i + S2_HEAD_SIZE)

    print(f"\n--- stage-4 flash survey at 0x{i:05X} ---")
    print(f"  launch_ms={launch_ms} last_error={last_error} "
          f"({ERRORS.get(last_error, '?')}) launch_kind={kind}")
    names = "|".join(n for b, n in RESETREAS_BITS if resetreas & b) or "none"
    print(f"  resetreas=0x{resetreas:08X} ({names})")

    print("\n  UICR (read only, never written):")
    print(f"    APPROTECT      = 0x{approtect:08X}"
          + ("  -> protected (0xFFFFFFFF is unprotected)" if approtect != 0xFFFFFFFF else "  -> unprotected"))
    print(f"    NRFFW[0]       = 0x{nrffw0:08X}"
          + ("  -> no bootloader address stored" if nrffw0 == 0xFFFFFFFF else "  -> bootloader start"))
    print(f"    NRFFW[1]       = 0x{bootaddr:08X}")

    regions = ((r0s, r0e, "vendor bootloader / MBR"), (r1s, r1e, "above the application image"))
    print(f"\n  page size {page_size}, {surveyed} pages surveyed, "
          f"{erased} erased (all 0xFF), {zero} all-zero")

    slot = 0
    for n, (start, end, label) in enumerate(regions):
        count = (end - start) // page_size if page_size else 0
        sample = samples[n * 64:(n + 1) * 64]
        print(f"\n  region {n}: 0x{start:08X}-0x{end:08X}  {count} pages  ({label})")
        print(f"    first 32 bytes: {sample[:32].hex(' ')}")
        print(f"    ascii         : |{ascii_of(sample[:32])}|")
        if not any(sample):
            print("    >>> reads as all zero - either genuinely blank or the read was blocked")
        elif all(b == 0xFF for b in sample):
            print("    >>> reads as erased flash (0xFF)")
        else:
            print("    >>> HAS CONTENT")
        shown = []
        for k in range(count):
            if slot + k < S4_MAX_PAGES:
                crc = struct.unpack_from("<I", crcs, (slot + k) * 4)[0]
                shown.append(f"{start + k * page_size:08X}:{crc:08X}")
        for line_start in range(0, len(shown), 4):
            print("      " + "  ".join(shown[line_start:line_start + 4]))
        slot += count

    print("\n  >>> READING")
    if not any(samples[:64]):
        print("      Bootloader region reads as zero. Either it is genuinely")
        print("      blank (it is not - the device boots) or reads are blocked.")
    else:
        print("      Bootloader region is READABLE from the payload. A full dump")
        print("      is possible, which would give the backup this project has")
        print("      never had.")


def print_s3(data: bytes, i: int, version: int = 6) -> None:
    (_magic, _ver, stage, launch_ms, last_error, bringup_ok,
     enable_to_ready, resetreas, kind) = struct.unpack_from(S2_HEAD, data, i)

    print(f"\n--- stage-3 record at 0x{i:05X} ---")
    print(f"  stage={stage} launch_ms={launch_ms} bringup_ok={bringup_ok}")
    print(f"  last_error={last_error} ({ERRORS.get(last_error, '?')})")
    names = "|".join(n for b, n in RESETREAS_BITS if resetreas & b) or "none"
    print(f"  resetreas=0x{resetreas:08X} ({names})  launch_kind={kind}")
    print(f"  enable->ready = {enable_to_ready} cycles ({enable_to_ready/64.0:.1f} us)")

    off = i + S2_HEAD_SIZE
    run, matched, mm_index, mm_offset = struct.unpack_from(S3_SUMMARY, data, off)
    off += struct.calcsize(S3_SUMMARY)
    ok_replay = matched == PREFIX_FRAMES and mm_index == NO_INDEX
    print(f"\n  replay: {matched}/{PREFIX_FRAMES} matched, {run} run -> "
          f"{'OK' if ok_replay else f'FAILED at frame {mm_index}, byte {mm_offset}'}")

    rx, _txa, _rxa, _ev, res, _wtx, _wrx = struct.unpack_from(S1_EXCH, data, off)
    off += struct.calcsize(S1_EXCH)
    configured = res == 0 and rx[0] == 0xA0 and rx[1] == 1
    print(f"  post-replay 0xA0: rx[0:4]={rx[:4].hex(' ')} -> "
          f"{'CONFIGURED' if configured else 'NOT configured - poll results below mean nothing'}")

    window_us, polls_a0, polls_a1, attn_seen, events_total, lost = struct.unpack_from(
        S3_STATS, data, off)
    off += struct.calcsize(S3_STATS)
    print(f"\n  poll window {window_us/1e6:.1f} s: a0={polls_a0} a1={polls_a1} "
          f"attn_high_seen={attn_seen} events={events_total} a0_lost_config={lost}")
    if lost:
        print("    !! the STM32 dropped its configuration mid-window; an absence")
        print("       of key events here is NOT evidence that nothing was pressed")

    scan_map = load_scan_map()
    observed: list[str] = []
    shown = min(events_total, S3_EVENTS)
    print(f"\n  captured events ({shown} of {events_total}):")
    for n in range(shown):
        t_us, result, evrx = struct.unpack_from(S3_EVENT, data, off + n * struct.calcsize(S3_EVENT))
        bitmap = evrx[:BITMAP_BYTES]
        status = evrx[BITMAP_BYTES]
        bits = bits_set(bitmap)
        named = [scan_map.get(b, f"bit{b}") for b in bits]
        label = ", ".join(named) if named else "(all clear - release)"
        print(f"    [{n}] t={t_us/1000.0:8.1f} ms  result={result} "
              f"bitmap={bitmap.hex(' ')} status=0x{status:02X}")
        print(f"         bits={bits} -> {label}")
        if len(bits) == 1:
            observed.append(named[0])

    if version in (11, 12):
        (rc, calls, ok, ble_conn, transport, kscan_ready) = struct.unpack_from(
            S3_DIAG, data, i + S3_SIZE)
        print(f"\n  INGEST into ZMK: calls={calls} ok={ok} last_rc={rc}"
              + ("  (-13 = EACCES)" if rc == -13 else ""))
        print(f"  kscan device ready   : {'yes' if kscan_ready else 'NO'}")
        print(f"  BLE profile connected: {'yes' if ble_conn else 'NO'}")
        # enum zmk_transport from app/include/zmk/endpoints_types.h:
        # NONE=0, USB=1, BLE=2.
        transports = {0: "NONE", 1: "USB", 2: "BLE"}
        print(f"  selected endpoint    : {transport} "
              f"({transports.get(transport, 'unknown')})")
        if rc == -13:
            print("    >>> ZMK never enabled the kscan device. Keys were read but")
            print("        never entered the HID pipeline.")
        elif calls and ok == calls and not ble_conn:
            print("    >>> ingest succeeded, but ZMK has no connected BLE profile,")
            print("        so it has nowhere to send the report. If this follows a")
            print("        reflash, the host link may simply not have re-established")
            print("        yet - give it a few seconds and press again.")
        elif calls and ok == calls and ble_conn and transport == 2:
            print("    >>> ingest succeeded, a profile is connected, and the")
            print("        selected endpoint is BLE. Keys should be reaching the")
            print("        host - confirmed typing on 2026-08-04.")

    # 38 is the pin-survey variant of 37: same 1280-byte record, and every
    # block below is separately gated, so it simply skips the ones that do not
    # apply to it. Leaving it out of this outer list silently skipped the whole
    # section - the survey data was in the record and nothing printed it.
    if version in (12, 13, 14, 15, 16, 17, 18, 19, 32, 34, 35, 36, 37, 38, 39):
        regstatus, usb_status, conn = struct.unpack_from(
            S3_USB, data, i + S3_DIAG_SIZE)
        vbus = regstatus & 1
        outrdy = (regstatus >> 1) & 1
        print(f"\n  USB: POWER->USBREGSTATUS=0x{regstatus:08X}  "
              f"VBUSDETECT={vbus}  OUTPUTRDY={outrdy}")
        print(f"       zmk_usb_get_status()     = {usb_status} "
              f"({USB_DC_STATUS.get(usb_status, '?')})")
        print(f"       zmk_usb_get_conn_state() = {conn} "
              f"({USB_CONN_STATE.get(conn, '?')})")
        if version in (13, 14, 15, 16, 17, 18, 19, 32, 34, 35, 36, 37):
            en, pullup, cause, hfclk = struct.unpack_from(
                S3_USBD, data, i + S3_USB_SIZE)
            print(f"       USBD->ENABLE={en}  USBD->USBPULLUP={pullup}  "
                  f"EVENTCAUSE=0x{cause:08X}")
            print(f"       CLOCK->HFCLKSTAT=0x{hfclk:08X}  "
                  f"SRC={'XTAL' if hfclk & 1 else 'RC'}  "
                  f"RUNNING={(hfclk >> 16) & 1}")

        if version in (14, 15, 16, 17, 18, 19, 32, 34, 35, 36, 37):
            (kick,) = struct.unpack_from(S3_KICK, data, i + S3_USBD_SIZE)
            print(f"       USBPWRRDY re-post: {kick} "
                  f"({USB_KICK.get(kick, '?')})")

        if version in (15, 16, 17, 18, 19, 32, 34, 35, 36, 37):
            (evb, eva, inten, nvic, pua) = struct.unpack_from(
                S3_IRQ, data, i + S3_KICK_SIZE)
            print(f"       around the re-post: EVENTS_USBPWRRDY {evb} -> {eva}"
                  f"   USBPULLUP after = {pua}")
            print(f"       POWER->INTENSET=0x{inten:08X}  "
                  f"NVIC POWER_CLOCK enabled={nvic}")
            if kick == 0 and eva:
                print("    >>> THE EVENT LATCHED AND NOTHING CONSUMED IT. The")
                print("        interrupt is not being delivered, so re-posting can")
                print("        never work - the driver must be advanced another way.")
            elif kick == 0 and not evb and not eva and not pua:
                print("    >>> The write did not latch. EVENTS_USBPWRRDY reads 0")
                print("        immediately after being written.")
            elif kick == 0 and not eva and pua:
                print("    >>> THE RE-POST WORKED. The ISR consumed the event and")
                print("        the pullup came up within 20 ms.")
            elif kick == 0 and not eva and not pua:
                print("    >>> The ISR consumed the event but the pullup did not")
                print("        come up - the fault is past the power handler.")

        if version in (16, 17, 18, 19, 32, 34, 35, 36, 37):
            (en_rc, usbd_inten, pu_final) = struct.unpack_from(
                S3_EN, data, i + S3_IRQ_SIZE)
            print(f"       usb_enable() second call = {en_rc} "
                  f"({'-EALREADY, stack believes USB is enabled' if en_rc == -120 else 'see errno'})")
            print(f"       USBD->INTEN=0x{usbd_inten:08X}   "
                  f"USBPULLUP after second enable = {pu_final}")
            if pu_final:
                print("    >>> THE SECOND ENABLE BROUGHT THE PULLUP UP. The first")
                print("        one never attached; the power events were a red herring.")
            elif usbd_inten == 0:
                print("    >>> USBD->INTEN is 0: nrf_usbd_common_start() has never")
                print("        run, which is consistent with USBD_POWERED never")
                print("        being processed.")

        # kick != 0 means the re-post body never executed, so every field it
        # would have written is still zero-initialised. Printing them anyway
        # produced confident nonsense - a capture from a working, CONFIGURED
        # USB link decoded as "USBD does not start on this die" and "even in
        # normal mode the pullup will not set", both read straight out of
        # never-written memory. Only report these when the body actually ran.
        if version in (17, 18, 19, 32, 34, 35, 36, 37) and kick == 0:
            (pre,) = struct.unpack_from(S3_PRE, data, i + S3_EN_SIZE)
            print(f"       USBD->ENABLE before the USB stack ran = {pre} "
                  f"({'loader handed over a live USBD; cold reset applied' if pre else 'already cold'})")

        if version in (18, 19, 32, 34, 35, 36, 37) and kick == 0:
            (lp, forced, forced1s, st_forced) = struct.unpack_from(
                S3_FORCE, data, i + S3_PRE_SIZE)
            print(f"       USBD->LOWPOWER={lp}   forced USBPULLUP write -> "
                  f"reads back {forced}, still {forced1s} after 1 s")
            print(f"       zmk_usb_get_status() 1 s after forcing = {st_forced} "
                  f"({USB_DC_STATUS.get(st_forced, '?')})")
            if version in (19, 32, 34, 35, 36, 37):
                pass
            elif not forced:
                print("    >>> THE HARDWARE REFUSES THE WRITE. USBPULLUP reads 0")
                print("        immediately after being written 1, with USBD")
                print("        enabled. No driver-side work can fix this; the")
                print("        peripheral is not in a state that accepts attach.")
            elif forced and not forced1s:
                print("    >>> The write took and was then UNDONE within 1 s -")
                print("        something is actively clearing the pullup.")
            elif st_forced == 3:
                print("    >>> FORCING THE PULLUP ENUMERATED THE DEVICE. The host")
                print("        CONFIGURED it - the wire is fine and the fault was")
                print("        entirely in the driver's attach path.")
            else:
                print("    >>> Pullup is asserted and held, and the host has not")
                print("        answered. That points at the data lines, not the")
                print("        firmware - confirm on the host side.")

        if version in (19, 32, 34, 35, 36, 37) and kick == 0:
            (lp_after, pu_norm, pu_forced_norm, st_norm) = struct.unpack_from(
                S3_NORM, data, i + S3_FORCE_SIZE)
            print(f"       out of low power: LOWPOWER={lp_after}   "
                  f"USBPULLUP reads {pu_norm}, {pu_forced_norm} after forcing")
            print(f"       zmk_usb_get_status() 2 s later = {st_norm} "
                  f"({USB_DC_STATUS.get(st_norm, '?')})")
            if lp_after:
                print("    >>> USBD will not leave low power. It is being held")
                print("        there, or the write is inert for another reason.")
            elif pu_norm:
                print("    >>> THE PULLUP WAS ASSERTED ALL ALONG. The device did")
                print("        attach; the host never answered and the driver")
                print("        suspended. The fault is on the wire - the data")
                print("        lines are not reaching the host while we run.")
            elif pu_forced_norm and st_norm == 3:
                print("    >>> FORCING IT IN NORMAL MODE ENUMERATED THE DEVICE.")
            elif pu_forced_norm:
                print("    >>> Pullup now asserted and held; the host still has not")
                print("        answered. Points at the data lines, not the driver.")
            else:
                print("    >>> Even in normal mode the pullup will not set. The")
                print("        peripheral is not in a state that accepts attach.")

        if version in (32, 34, 35, 36, 37):
            snap = struct.unpack_from(S3_SNAP, data, i + S3_NORM_SIZE)
            print("")
            print("       USBD/CLOCK snapshots")
            print("         " + "register".ljust(13)
                  + "".join(w.ljust(20) for w in SNAP_WHEN))
            for w, nm in enumerate(SNAP_NAMES):
                cells = "".join(f"0x{snap[s * S3_SNAP_WORDS + w]:08X}".ljust(20)
                                for s in range(S3_SNAPS))
                print(f"         {nm.ljust(13)}{cells}")
            hf = [snap[s * S3_SNAP_WORDS + 10] for s in range(S3_SNAPS)]
            pu = [snap[s * S3_SNAP_WORDS + 1] for s in range(S3_SNAPS)]
            if not (hf[0] & 1) and (hf[1] & 1):
                print("    >>> HFXO was NOT the crystal at stage entry and is now.")
            if pu[2]:
                print("    >>> With HFXO forced and LOWPOWER clear, the pullup TOOK.")
            elif pu[0] or pu[1]:
                print("    >>> The pullup was asserted at some point.")

        if version in (32, 34, 35, 36, 37) and kick == 0:
            (rdy, spins, re_pu) = struct.unpack_from(
                S3_RE, data, i + S3_SNAP_SIZE)
            print(f"       USBD ENABLE cycle: EVENTCAUSE.READY={rdy} after "
                  f"{spins} spins, USBPULLUP reads {re_pu}")
            if re_pu:
                print("    >>> RE-ENABLING USBD MADE THE PULLUP TAKE. The"
                      " peripheral")
                print("        was enabled before its regulator was ready and"
                      " stayed")
                print("        wedged. The re-enable sequence recovered it.")
            elif not rdy:
                print("    >>> USBD never signalled READY after being re-enabled.")
                print("        The peripheral will not start at all.")
            else:
                print("    >>> USBD reports READY and STILL refuses the pullup.")
                print("        Nothing software-side is left; treat as hardware.")

        if version in (32, 34, 35, 36, 37) and kick == 0:
            (v1, v2, e187, e171, f_rdy, f_spins, f_pu) = struct.unpack_from(
                S3_ERR, data, i + S3_RE_SIZE)
            print(f"       errata gate: 0x10000130=0x{v1:08X} "
                  f"0x10000134=0x{v2:08X}  "
                  f"nrf52_errata_187()={e187}  171()={e171}")
            print(f"       with 171+187 forced: EVENTCAUSE.READY={f_rdy} after "
                  f"{f_spins} spins, USBPULLUP reads {f_pu}")
            if f_pu:
                print("    >>> APPLYING THE ERRATA WORKAROUNDS BY HAND BROUGHT")
                print("        USBD UP AND THE PULLUP TOOK. The runtime revision")
                print("        gate is skipping a workaround this die needs.")
            elif f_rdy:
                print("    >>> USBD signalled READY with the workarounds forced,")
                print("        but the pullup still will not set.")
            elif not e187 and v1 != 0x0D:
                print("    >>> The gate skipped erratum 187 (revision word is not")
                print("        0x0D) and forcing it did not help either. USBD")
                print("        does not start on this die.")

        if version in (32, 34, 35, 36, 37):
            (n, pu_seen, pu_ms, epin, epout, lp_ms, ec_or) = struct.unpack_from(
                S3_EARLY, data, i + S3_ERR_SIZE)
            never = 0xFFFFFFFF
            print("")
            print(f"       EARLY WINDOW ({n} samples, 1 ms apart, from just")
            print( "       after ZMK usb_enable() - before the window shuts)")
            print(f"         USBPULLUP ever set : "
                  f"{'YES at %d ms' % pu_ms if pu_seen else 'NO'}")
            print(f"         EPINEN max         : 0x{epin:08X}   "
                  f"EPOUTEN max: 0x{epout:08X}   (reset value is 1)")
            print(f"         LOWPOWER first set : "
                  f"{'%d ms' % lp_ms if lp_ms != never else 'never'}")
            print(f"         EVENTCAUSE (OR)    : 0x{ec_or:08X}")
            if pu_seen:
                print("    >>> THE PULLUP DID ASSERT. The device attached and the")
                print("        host did not answer. Every later USBPULLUP=0 was a")
                print("        dead-window read. The fault is on the wire.")
            elif epin or epout:
                print("    >>> The window was open (EPINEN/EPOUTEN readable) and the")
                print("        pullup still never asserted - the fault is in the")
                print("        driver's attach path after all.")
            else:
                print("    >>> The window was ALREADY shut this early: EPINEN and")
                print("        EPOUTEN read below their reset value of 1 from the")
                print("        first sample. Sample earlier or from the ISR.")

        if version in (32, 34, 35, 36, 37):
            (lp_b, pu_lo, pu_hi, st, ec) = struct.unpack_from(
                S3_RE2, data, i + S3_EARLY_SIZE)
            print("")
            print("       DELIBERATE RE-ATTACH (pullup dropped 400 ms, re-raised)")
            print(f"         LOWPOWER before    : {lp_b}")
            print(f"         USBPULLUP after 0  : {pu_lo}    after 1: {pu_hi}")
            print(f"         EVENTCAUSE (OR) 2 s: 0x{ec:08X}")
            print(f"         status 2 s after   : {st} "
                  f"({USB_DC_STATUS.get(st, '?')})")
            if st in (1, 2, 3):
                print("    >>> THE HOST ANSWERED THE SECOND EDGE. The first attach")
                print("        landed inside the host's disconnect handling and was")
                print("        missed. Diagnosis and fix: re-attach after boot.")
            elif pu_hi != 1:
                print("    >>> Could not re-raise the pullup - the register window")
                print("        was shut again. Test inconclusive.")
            else:
                print("    >>> A clean second edge, well clear of the disconnect,")
                print("        and the host STILL did not answer. The host is not")
                print("        missing an edge: suspect the physical D+ path.")

        if version in (32, 34, 35, 36, 37):
            (dis_rc, en_rc2, pu2, pu2_ms) = struct.unpack_from(
                S3_RE3, data, i + S3_RE2_SIZE)
            print("")
            print("       DRIVER-LEVEL RE-ATTACH (usb_disable, 500 ms, usb_enable)")
            print(f"         usb_disable() = {dis_rc}   usb_enable() = {en_rc2}")
            print(f"         pullup re-asserted: "
                  f"{'YES at %d ms' % pu2_ms if pu2 else 'NO'}")
            if pu2:
                print("    >>> A CLEAN SECOND D+ EDGE WAS PRODUCED, ~2.5 s after")
                print("        boot and well clear of the host's disconnect")
                print("        handling. Whether the host answered is decided on")
                print("        the host side, not here.")
            else:
                print("    >>> The re-attach did not re-assert the pullup, so no")
                print("        second edge was produced. Test inconclusive again.")

        if version in (32, 34, 35, 36, 37):
            pwr = struct.unpack_from(S3_PWR, data, i + S3_RE3_SIZE)
            print("")
            print("       PRE-USB POWER / CLOCK")
            print("         " + "register".ljust(14)
                  + "".join(w.ljust(28) for w in PWR_WHEN))
            for w, nm in enumerate(PWR_NAMES):
                cells = "".join(f"0x{pwr[s * S3_PWR_WORDS + w]:08X}".ljust(28)
                                for s in range(2))
                print(f"         {nm.ljust(14)}{cells}")
            dcdc0, dcdc1 = pwr[4], pwr[S3_PWR_WORDS + 4]
            hv0, hv1 = pwr[5], pwr[S3_PWR_WORDS + 5]
            if not dcdc0 and not dcdc1:
                print("    >>> DCDCEN is 0 both before and after: the regulator")
                print("        config the loader used for its USB session did not")
                print("        survive the SYSRESETREQ, and nothing restores it.")
            elif dcdc0 or hv0:
                print("    >>> Power config WAS inherited - it survived the reset,")
                print("        so this asymmetry is not the explanation.")

        if version in (32, 34, 35, 36, 37):
            (rw_b, rw_a, rw_ms) = struct.unpack_from(
                S3_RW, data, i + S3_PWR_SIZE)
            print("")
            print("       USB REGULATOR WAIT (APPLICATION 95, before usb_enable)")
            print(f"         USBREGSTATUS  0x{rw_b:08X} -> 0x{rw_a:08X}")
            print(f"         waited        "
                  + ("TIMED OUT (rail never became ready)" if rw_ms == 0xFFFFFFFF
                     else f"{rw_ms} ms"))
            if rw_ms == 0xFFFFFFFF:
                print("    >>> The USB regulator never became ready. That is a")
                print("        different fault from the one this addresses.")
            elif not (rw_b & 2) and (rw_a & 2):
                print("    >>> CONFIRMED THE DIAGNOSIS: the rail was NOT ready when")
                print(f"        the USB stack would have attached, and took {rw_ms} ms.")
                print("        USB now attaches into a live supply.")
            elif rw_b & 2:
                print("    >>> The rail was ALREADY ready on entry, so on this launch")
                print("        the wait changed nothing - the timing differs per boot.")

        if version in (32, 34, 35, 36, 37):
            (tt, te, tl, tv, tw) = struct.unpack_from(
                S3_TRIG, data, i + S3_RW_SIZE)
            if tt or te or tl or tv or tw:
                print("")
                print("       WHICH WRITE STARTS THE REGULATOR")
                print(f"         after errata trim window : 0x{tt:08X}")
                print(f"         after USBD ENABLE cycle  : 0x{te:08X}")
                print(f"         after LOWPOWER = 0       : 0x{tl:08X}")
                print(f"         after EVENTS_USBPWRRDY   : 0x{tv:08X}")
                print(f"         verdict: {tw} ({TRIG_WHICH.get(tw, '?')})")
                if tw:
                    print("    >>> FOUND THE TRIGGER. Doing this before ZMK's")
                    print("        usb_enable() should let USB attach into a live")
                    print("        rail - that is the fix.")
                else:
                    print("    >>> None of our writes started it either, so the 0x3")
                    print("        seen at stage entry comes from somewhere else.")

        if version in (32, 34, 35, 36, 37) and kick == 0:
            (hr, hs, he) = struct.unpack_from(S3_HOLD, data, i + S3_TRIG_SIZE)
            print("")
            print("       D+ HELD UP FOR 5 s - DID THE HOST ANSWER?")
            print(f"         EVENTS_USBRESET (OR) : 0x{hr:08X}")
            print(f"         EVENTS_EP0SETUP (OR) : 0x{hs:08X}")
            print(f"         EVENTS_USBEVENT (OR) : 0x{he:08X}")
            if hr or hs:
                print("    >>> THE HOST ANSWERED. D+ does reach it, and the fault is")
                print("        the driver suspending 3 ms after attach - inside the")
                print("        host's connect debounce. Suppress the suspend until")
                print("        after the first USBRESET.")
            else:
                print("    >>> FIVE SECONDS OF ASSERTED D+ AND NO HOST RESPONSE AT")
                print("        ALL. Not a timing problem. D+ is not reaching the")
                print("        host - stop debugging firmware and measure the pin.")

        if version in (32, 34, 35, 36, 37):
            (cnf_b, cnf_a, p_in) = struct.unpack_from(
                S3_P26, data, i + S3_HOLD_SIZE)
            print("")
            print("       P0.26 DRIVEN HIGH BEFORE usb_enable()")
            print(f"         PIN_CNF[26]  before 0x{cnf_b:08X}  after 0x{cnf_a:08X}")
            print(f"         P0.IN        0x{p_in:08X}   bit26 = {(p_in >> 26) & 1}")

        if version in (32, 34, 35, 36, 37):
            (win, frames_a1, seen, first, bmap, mode_class,
             mode_mv) = struct.unpack_from(
                S3_CHORD, data, i + S3_P26_SIZE)
            names = load_scan_map()
            bits = [names.get(b, str(b)) for b in bits_set(bmap)]
            print("")
            print("       ESCAPE-HATCH CHORD (ESC held at boot) - DETECTION ONLY")
            print(f"         window            {win} ms, {frames_a1} 0xA1 frames")
            print(f"         chord seen        "
                  + (f"YES at {first} ms" if seen else "no"))
            print(f"         keys seen at all  {bits if bits else 'none'}")
            if seen:
                print("    >>> DETECTED. The escape hatch is viable: this is a")
                print("        deterministic way back to the loader that does not")
                print("        depend on the payload being healthy.")
            elif frames_a1 == 0:
                print("    >>> NO 0xA1 FRAMES AT ALL in the window. Either no key")
                print("        was held, or a held-from-before-boot key produces no")
                print("        event - which would make this approach unworkable and")
                print("        is exactly what the test exists to find out.")
            else:
                print("    >>> Frames arrived but the chord bit was never set. If a")
                print("        key WAS held, the held-key-produces-no-event problem")
                print("        is real and the hatch needs a different trigger.")

        if version in (32, 34, 35, 36, 37):
            (polls, first, b2or, low6, plateau, last) = struct.unpack_from(
                S3_TRAVEL, data, i + S3_CHORD_SIZE)
            never = 0xFFFFFFFF
            print("")
            print("       0xA0 TRAVEL SCALAR, whole run (byte[2]: bit7 = key down,")
            print("       bits0-5 = 0..63 magnitude)")
            print(f"         polls            {polls}")
            print(f"         key-down level   "
                  + (f"first at {first} ms" if first != never else "NEVER"))
            print(f"         byte[2] OR       0x{b2or:02X}   last 0x{last:02X}"
                  f"   max magnitude {low6}")
            print(f"         0x40 plateau     {plateau} samples"
                  + (f" ({100 * plateau // polls}% of polls)" if polls else ""))
            if polls and plateau * 10 > polls * 9:
                print("    >>> byte[2] PINNED AT 0x40 for the whole run. Stock idle")
                print("        captures show a mix (0x01/0x02/0x03/0x07 as well as")
                print("        0x40), so this is a different scanner state - but if")
                print("        no key was pressed this run, it does NOT by itself")
                print("        show the scanner is unable to report. Hold a key to")
                print("        distinguish: bit 7 setting proves the level-based")
                print("        hatch works; byte[2] staying 0x40 proves it cannot.")
            elif first != never:
                print("    >>> KEY-DOWN LEVEL SEEN. The level-based hatch is viable:")
                print("        it does not depend on an edge a held key may never make.")
            elif polls:
                print("    >>> Scanner reporting normally (no plateau) but no key-down")
                print("        level seen - consistent with nothing being held.")

        if version in (34, 35, 36, 37):
            (feeds, up, crv, rren, cfg, stop) = struct.unpack_from(
                S3_WDT, data, i + S3_TRAVEL_SIZE)
            print("")
            print("       WATCHDOG (time-bounded feed)")
            print(f"         feeds {feeds}   uptime at record {up} ms")
            print(f"         CRV 0x{crv:08X}  RREN 0x{rren:08X}  CONFIG 0x{cfg:08X}")
            print(f"         {stop} ({WDT_STOP.get(stop, '?')})")

            # MODE SWITCH, filled at the same instant as the watchdog fields
            # above by s3_fill_live(). mode_class/mode_mv come from the chord
            # unpack; a zero mode_class means a pre-telemetry image, which is
            # why the firmware always stamps PRESENT.
            if version == 37 and (mode_class & MODE_CLASS_PRESENT):
                # mode_sample_ms is the last word of the SPIM2 block (was
                # spim2_reserved). That block is unpacked further down, so read
                # just this field here rather than reorder the printing.
                (mode_sample_ms,) = struct.unpack_from(
                    "<I", data, i + S3_SPIM2_SIZE - 4)
                cls = mode_class & MODE_CLASS_MASK
                seen_hi = bool(mode_class & MODE_SEEN_HIGH)
                seen_lo = bool(mode_class & MODE_SEEN_LOW)
                age = up - mode_sample_ms
                print(f"         mode {MODE_NAME.get(cls, '?')} "
                      f"({cls}) at {mode_mv} mV   "
                      f"thresholds USB>={MODE_USB_MIN_MV} "
                      f"DONGLE>={MODE_DONGLE_MIN_MV}")
                print(f"         last mode sample {mode_sample_ms} ms "
                      f"(age {age} ms)"
                      + ("   <<< SAMPLER STALLED" if age > 5000 else ""))
                swing = ("moved across the threshold this run"
                         if (seen_hi and seen_lo) else
                         "NEVER left the top rail" if seen_hi else
                         "never reached the dongle level")
                print(f"         line history: {swing}")
                if stop == 3:
                    # The feed was withheld for DONGLE. Say which explanation
                    # the evidence supports, since they need opposite fixes.
                    if seen_hi and seen_lo:
                        print("    >>> The line demonstrably swings, so this is a"
                              " REAL switch position.")
                        print("        The device is heading for recovery because"
                              " it was asked to.")
                    else:
                        print("    >>> The line has NEVER been below the dongle"
                              " threshold this run.")
                        print("        Either the switch sat in dongle the whole"
                              " time, or the pin is stuck")
                        print("        high / mis-mapped. Flip the switch during a"
                              " run to tell them apart:")
                        print("        if both SEEN bits then set, the hardware is"
                              " fine.")
                elif cls == 2:
                    # Mode reads dongle but the last feed decision predates it;
                    # wdt_stopped lags by up to one 15 s feed period.
                    print("    >>> Mode reads DONGLE but the last feed decision"
                          " predates it (lags up to 15 s).")
                    print("        Expect stop reason 3 in the next record.")

        if version in (34, 35, 36, 37):
            (startms,) = struct.unpack_from(S3_START, data, i + S3_WDT_SIZE)
            print(f"         USB watch began at uptime {startms} ms "
                  + ("(after workqueue start - samples the real attach)"
                     if startms else "(0 - still running before threads!)"))

        if version in (35, 36, 37):
            vals = struct.unpack_from(S3_AIN, data, i + S3_START_SIZE)
            lo, hi, last, n = vals[0:5], vals[5:10], vals[10:15], vals[15]
            print("")
            print(f"       MODE-SWITCH SWEEP  ({n} samples at 2 Hz)")
            print("         pin      min mV   max mV  last mV   spread")
            best = None
            for k, pin in enumerate(AIN_PINS):
                to_mv = lambda r: r * 3600 // 4096
                mn, mx, lt = to_mv(lo[k]), to_mv(hi[k]), to_mv(last[k])
                spread = mx - mn
                print(f"         {pin:7s} {mn:7d} {mx:8d} {lt:8d} {spread:8d}")
                if best is None or spread > best[1]:
                    best = (pin, spread, mn, mx)
            print("")
            if n == 0:
                print("    >>> NO SAMPLES. The sweep did not run.")
            elif best[1] >= 1200:
                print(f"    >>> {best[0]} swings {best[2]}..{best[3]} mV - that is the")
                print("        mode switch, provided all three positions were used")
                print("        during the window. 0 / ~1800 / ~3300 mV is the")
                print("        expected divider ladder.")
            else:
                print("    >>> No channel moved more than 1.2 V. Either the switch")
                print("        was not flipped during the window, or the mode pin")
                print("        is not among these five and the divider is read")
                print("        somewhere other than the nRF.")

        if version in (36, 37):
            (sck, mosi, miso, csn, en, freq, cfg,
             mode_sample_ms) = struct.unpack_from(
                S3_SPIM2, data, i + S3_AIN_SIZE)

            def psel(v):
                return "disconnected" if v & 0x80000000 else f"P{(v >> 5) & 1}.{v & 31:02d}"

            print("")
            print("       SPIM2 as the vendor loader left it (IS31FL3743B RGB)")
            print(f"         SCK  0x{sck:08X}  {psel(sck)}")
            print(f"         MOSI 0x{mosi:08X}  {psel(mosi)}")
            print(f"         MISO 0x{miso:08X}  {psel(miso)}")
            print(f"         CSN  0x{csn:08X}  {psel(csn)}")
            print(f"         ENABLE 0x{en:08X}  FREQUENCY 0x{freq:08X}  CONFIG 0x{cfg:08X}")
            if (sck | mosi) & 0x80000000:
                print("    >>> SCK or MOSI is disconnected: the loader did not leave")
                print("        SPIM2 configured, so the pins must come from elsewhere.")
            else:
                print(f"    >>> RGB bus is SCK {psel(sck)}, MOSI {psel(mosi)}, CS P0.11.")

        if version == 39:
            # Analog probe. Same offsets as the kbd_cap block again; a third
            # meaning. See G4B_EVIDENCE_VERSION_S3_ANALOG.
            kb = struct.unpack_from(S3_KBDCAP, data, i + S3_SPIM2_SIZE)
            frames, oob = kb[0], kb[1]
            h = kb[2:14]
            nring, blob = kb[15], kb[16]
            names = ("W", "A", "S", "D")
            print("")
            print("       ANALOG PROBE (0xA2 raw ADC, no HID)")
            print(f"         frames {frames}   rejected out-of-range {oob}")
            print("         key    min    max   last   span")
            spans = []
            for k, nm in enumerate(names):
                mn, mx, lastv = h[k * 3], h[k * 3 + 1], h[k * 3 + 2]
                span = mx - mn if mx > mn else 0
                spans.append(span)
                print(f"         {nm:4s} {mn:6d} {mx:6d} {lastv:6d} {span:6d}")
            # The ring is 4 keys x 11 uint16 packed across the [8][11] bytes.
            flat = bytes(b for row in blob for b in row) if isinstance(blob, (list, tuple)) else blob
            if nring:
                print(f"         first {min(nring, 11)} samples per key:")
                for k, nm in enumerate(names):
                    vals = []
                    for nn in range(min(nring, 11)):
                        o = (k * 11 + nn) * 2
                        if o + 1 < len(flat):
                            vals.append(flat[o] | (flat[o + 1] << 8))
                    print(f"           {nm}: " + " ".join(str(v) for v in vals))
            # Gamepad endpoint counters. Same three words the P0.26 block reads
            # for versions 32..37 (S3_P26 at S3_HOLD_SIZE); a version-39 record
            # means the gamepad, because only a GAMEPAD build writes them and
            # GAMEPAD selects the analog probe. No new version, so nothing had
            # to be added to the outer gate, the record dispatch or the stage-3
            # selector - 39 is already in all three.
            gpw, gpb, gpe = struct.unpack_from(S3_P26, data, i + S3_HOLD_SIZE)
            print("")
            print("       GAMEPAD HID ENDPOINT (second USB interface, HID_1)")
            print(f"         writes ok {gpw}   dropped busy {gpb}   errors {gpe}")
            if gpw == 0 and gpb == 0 and gpe == 0:
                print("    >>> All three zero. Either the image predates these")
                print("        counters (every capture before r27 reads zero here -")
                print("        the words were the retired P0.26 experiment), or this")
                print("        build has CONFIG_APEX_G4B_GAMEPAD off, or nothing was")
                print("        ever published - in which case the fault is upstream")
                print("        of USB and the frame count above says which: no 0xA2")
                print("        frames means there were no samples to publish.")
            elif gpw == 0 and gpe:
                print("    >>> EVERY WRITE FAILED. hid_int_ep_write() is refusing.")
                print("        -EAGAIN while unconfigured or suspended is normal, so")
                print("        this most likely means the host never configured the")
                print("        second interface - check CONFIG_USB_HID_DEVICE_COUNT=2.")
            elif gpw == 0 and gpb:
                print("    >>> EVERY REPORT WAS DROPPED AS BUSY. The first write never")
                print("        completed, so int_in_ready never fired and the endpoint")
                print("        is stalled. The host is not draining the interrupt IN.")
            elif gpb > gpw:
                print(f"    >>> More dropped ({gpb}) than sent ({gpw}). The publish rate")
                print("        outruns the endpoint. Not a fault - reports are absolute")
                print("        state, so a dropped one is superseded - but raise")
                print("        CONFIG_APEX_G4B_ANALOG_PERIOD_MS if it is extreme.")
            else:
                print("    >>> Reports are reaching the host. If the axis still looks")
                print("        dead in a game, the fault is in the mapping, not here.")

            if frames == 0:
                print("    >>> NO 0xA2 FRAMES COMPLETED. Either the opcode was")
                print("        rejected or every exchange failed - the analog path")
                print("        is not working and nothing below is meaningful.")
            elif max(spans) < 100:
                print("    >>> Values are being read but barely move. Either no key")
                print("        was pressed during the run, or the samples are not")
                print("        travel. Press W/A/S/D fully and re-run before")
                print("        designing an axis against this.")
            else:
                print("    >>> Usable travel seen. The largest span is the number an")
                print("        axis should normalise against; a key whose span is")
                print("        near zero was simply never pressed.")

        if version == 38:
            # Pin survey. Same offsets as the kbd_cap block; different meaning.
            kb = struct.unpack_from(S3_KBDCAP, data, i + S3_SPIM2_SIZE)
            p0_up, p0_dn = kb[0], kb[1]
            h = kb[2:14]
            p1_up = h[0] | (h[1] << 16)
            p1_dn = h[2] | (h[3] << 16)
            free, held = [], []
            for port, mask, up, dn in (("P0", SURVEY_P0_MASK, p0_up, p0_dn),
                                       ("P1", SURVEY_P1_MASK, p1_up, p1_dn)):
                for b in range(32):
                    if not (mask >> b) & 1:
                        continue
                    hi = (up >> b) & 1
                    lo = (dn >> b) & 1
                    name = f"{port}.{b:02d}"
                    if hi and not lo:
                        free.append(name)          # followed the pull both ways
                    else:
                        held.append(f"{name}={'hi' if hi and lo else 'lo'}")
            print("")
            print("       PIN SURVEY (read-only: pulled up, then down)")
            print(f"         FREE ({len(free)}): "
                  + (", ".join(free) if free else "none"))
            print(f"         held ({len(held)}): "
                  + (", ".join(held) if held else "none"))
            if len(free) >= 2:
                print("    >>> Enough free pins for I2C. Two of the above can be")
                print("        SDA and SCL. Confirm with a meter before soldering:")
                print("        a weak external pull can still lose to the internal one.")
            else:
                print("    >>> Fewer than two free pins. An I2C display would need")
                print("        one of the held pins investigated, or a pin shared")
                print("        with something already in use.")

        if version == 37:
            kb = struct.unpack_from(S3_KBDCAP, data, i + S3_SPIM2_SIZE)
            reads, ok = kb[0], kb[1]
            hist = kb[2:14]
            maxb, nsamp, blob = kb[14], kb[15], kb[16]
            keymap = load_scan_map()
            nonempty = sum(hist[1:])
            print("")
            print(f"       KEYBOARD-LOOP READ CAPTURE  (no ingest - cannot spam)")
            print(f"         0xA1 reads {reads}   SPIM-ok {ok}   max bits {maxb}")
            hb = "  ".join(f"{n}:{hist[n]}" for n in range(12) if hist[n])
            print(f"         popcount histogram (bits:count)  {hb}")
            if reads == 0:
                print("    >>> NO READS captured.")
            elif nonempty == 0:
                print("    >>> ALL reads empty (0 bits). The reads are CLEAN - the")
                print("        loop was NOT mis-reading garbage. The spam is downstream")
                print("        of the read (ingest / ZMK / repeat), not the SPI link.")
            else:
                print(f"    >>> {nonempty} of {ok} reads had bits set with NO key pressed.")
                print("        These are the spurious 'presses'. Samples:")
                for j in range(min(nsamp, 8)):
                    s = blob[j*11:j*11+11]
                    bm, st, res = s[:9], s[9], s[10]
                    keys = [keymap.get(b, f"#{b}") for b in bits_set(bm)]
                    print(f"          [{j}] bitmap={bm.hex(' ')} status=0x{st:02X} "
                          f"result={res} -> {', '.join(keys) or '(none)'}")

        if not vbus:
            print("    >>> VBUSDETECT is CLEAR: the chip does not see bus power.")
            print("        No driver configuration can enumerate from here - the")
            print("        cause is upstream of software. The keyboard's USB/BT")
            print("        selector switch is the obvious suspect.")
        elif vbus and not outrdy:
            print("    >>> VBUS present but the USB regulator is not ready.")
        elif version in (14, 15, 16, 17, 18) and pullup and usb_status == 3:
            print("    >>> USB IS ENUMERATED. Pullup is up and the host has")
            print("        CONFIGURED the device - the re-post worked.")
        elif version in (14, 15, 16, 17, 18) and pullup:
            print("    >>> ATTACH SIGNALLED - pullup is now up. The host has not")
            print(f"        reached CONFIGURED yet (status "
                  f"{USB_DC_STATUS.get(usb_status, '?')}).")
        elif version in (13, 14, 15, 16, 17, 18) and not en:
            print("    >>> USBD is DISABLED. The driver never brought the")
            print("        peripheral up despite usb_enable() being called.")
        elif version in (13, 14, 15, 16, 17, 18) and not pullup:
            print("    >>> PULLUP IS OFF. We never signalled attach, so the host")
            print("        cannot see the device. The fault is in the driver's")
            print("        attach path and is ours to fix.")
        elif version == 13 and pullup and usb_status == 5:
            print("    >>> PULLUP IS ON and the bus is idle. We signalled attach")
            print("        and the host did not answer - the fault is on the wire,")
            print("        not in the firmware. Suspect the data lines are not")
            print("        routed to the host while the payload is running.")
        elif conn == 0:
            print("    >>> VBUS present and regulated, but ZMK sees no connection -")
            print("        the device controller never attached.")
        else:
            print("    >>> USB is up.")

    print("\n  >>> ACCEPTANCE")
    if not configured:
        print("      FAIL - the replay did not take on this launch")
        return
    if lost:
        print("      FAIL - configuration lost mid-window")
        return
    if events_total < 2:
        print(f"      FAIL - {events_total} event(s); two distinct key presses are required")
        return

    want = list(EXPECTED_KEYS)
    order, cursor = [], 0
    for name in observed:
        if cursor < len(want) and name == want[cursor]:
            order.append(name)
            cursor += 1
    if cursor == len(want):
        print(f"      PASS - {' then '.join(order)} observed in order, each as a")
        print("      single-bit bitmap. Real keys are reaching the Nordic.")
    else:
        print(f"      FAIL - expected {' then '.join(want)}; single-bit events were "
              f"{observed or 'none'}")


def print_s2(data: bytes, i: int, version: int) -> None:
    (_magic, _ver, stage, launch_ms, last_error, bringup_ok,
     enable_to_ready, resetreas, kind) = struct.unpack_from(S2_HEAD, data, i)

    print(f"\n--- stage-2 record at 0x{i:05X} ---")
    print(f"  stage={stage} launch_ms={launch_ms} bringup_ok={bringup_ok}")
    print(f"  last_error={last_error} ({ERRORS.get(last_error, '?')})")
    print(f"  enable->ready = {enable_to_ready} cycles ({enable_to_ready/64.0:.1f} us)")
    names = "|".join(n for b, n in RESETREAS_BITS if resetreas & b) or "none"
    print(f"  resetreas=0x{resetreas:08X} ({names})")
    print(f"  launch_kind={kind} - {LAUNCH_KIND.get(kind, '?')}")

    if version in (3, 5):
        (run, matched, mm_index, mm_offset, results, _pad,
         expected, actual, verbatim) = struct.unpack_from(S2_REPLAY, data, i + S2_HEAD_SIZE)

        print(f"\n  REPLAY: {matched}/{PREFIX_FRAMES} frames matched, {run} run")
        print("  per-frame (M=match X=mismatch E=spim-error .=not run):")
        line = "".join(FRAME_RESULT.get(r, "?") for r in results)
        for start in range(0, PREFIX_FRAMES, 30):
            print(f"    {start:2d}: {line[start:start + 30]}")

        if mm_index == NO_INDEX:
            ok = matched == PREFIX_FRAMES
            print(f"\n  >>> {'PASS - all 59 frames matched' if ok else 'INCOMPLETE - no mismatch, but not all frames ran'}")
        else:
            off = None if mm_offset == NO_INDEX else mm_offset
            print(f"\n  >>> FIRST MISMATCH at frame {mm_index}"
                  + (f", byte offset {off}" if off is not None else " (SPIM error, no byte offset)"))
            print(f"    expected {expected[:16].hex(' ')} |{ascii_of(expected[:16])}|")
            print(f"    actual   {actual[:16].hex(' ')} |{ascii_of(actual[:16])}|")
            if off is not None and off < 16:
                print(f"    differs at byte {off}: "
                      f"expected 0x{expected[off]:02X}, got 0x{actual[off]:02X}")

        print("\n  landmark frames:")
        for n, idx in enumerate(VERBATIM_INDEX):
            frame = verbatim[n * 64:(n + 1) * 64]
            note = {0: "oracle, expect '3.24.1'", 29: "expect 20 00 00",
                    57: "expect 20 02 02", 58: "expect 00x9 40"}[idx]
            # 12 bytes, not 8: frame 58's landmark is the 0x40 at byte 9.
            state = "not reached" if not any(frame) else frame[:12].hex(' ')
            print(f"    frame {idx:2d} ({note}): {state}  |{ascii_of(frame[:12])}|")

        if version == 3:
            print("\n  (version 3 record: no post-replay probe in this build)")
            return

        rx, txa, rxa, ev, res, wtx, wrx = struct.unpack_from(
            S1_EXCH, data, i + S2_REPLAY_SIZE)
        print("\n  POST-REPLAY 0xA0 - the positive control, sent on this launch")
        print(f"    result={res} ({SPIM_RESULT.get(res, '?')})  "
              f"tx_amount={txa} rx_amount={rxa} events_end={ev}")
        print(f"    rx[0:16] = {rx[:16].hex(' ')}  |{ascii_of(rx[:16])}|")
        if res != 0:
            print("    >>> INCONCLUSIVE: the exchange itself failed")
        elif rx[0] != 0xA0:
            print("    >>> INCONCLUSIVE: no 0xA0 echo at rx[0]")
        elif rx[1] == 1:
            print("    >>> rx[1]=1 - CONFIGURED right after the replay.")
            print("        The field is meaningful, and rx[1]=0 on the relaunches")
            print("        means the configuration really is lost across a Nordic")
            print("        reset. G4B-3 must replay before polling keys.")
        else:
            print(f"    >>> rx[1]={rx[1]} - NOT configured even immediately after a")
            print("        successful replay. rx[1] does not mean 'configured'.")
            print("        Every inference drawn from that field needs revisiting.")
        return

    opcodes = struct.unpack_from(S2_CONFIRM, data, i + S2_HEAD_SIZE)[:2]
    base = i + S2_HEAD_SIZE + struct.calcsize(S2_CONFIRM)
    print("\n  CONFIRM: read-only. rx[1] of 0xA0 says whether the STM32 kept its")
    print("  configuration across this Nordic reset - what G4B-3 needs to know.")
    for n, opcode in enumerate(opcodes):
        rx, txa, rxa, ev, res, wtx, wrx = struct.unpack_from(
            S1_EXCH, data, base + n * struct.calcsize(S1_EXCH))
        print(f"\n  exchange {n}: TX opcode 0x{opcode:02X}")
        print(f"    result={res} ({SPIM_RESULT.get(res, '?')})  "
              f"tx_amount={txa} rx_amount={rxa} events_end={ev}")
        print(f"    rx[0:16] = {rx[:16].hex(' ')}  |{ascii_of(rx[:16])}|")
        if opcode == 0x90:
            print(f"    >>> ORACLE: {'PASS' if rx[:6] == b'3.24.1' else 'FAIL'}")
        elif opcode == 0xA0 and rx[0] == 0xA0:
            print(f"    >>> rx[1]={rx[1]} - STM32 "
                  f"{'IS still configured' if rx[1] == 1 else 'is NOT configured'}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()

    data = args.capture.read_bytes()
    print(f"capture={args.capture}  bytes={len(data)}  record_size={RECORD_SIZE}")

    # Wrapper beacons: how many launches, and why each reset. The 18-byte
    # diagnostic form adds GPREGRET2 and the RAM marker as read on entry; the
    # two forms are told apart by where the CRLF lands.
    beacons, off = [], 0
    while True:
        i = data.find(APXB_MAGIC, off)
        if i < 0:
            break
        off = i + 1
        # Short form first: in the 18-byte form, +8/+9 are the GPREGRET2 low
        # byte and zero (only the low 8 bits exist), so they can never be CRLF,
        # whereas traffic after a 10-byte beacon could put CRLF at +16.
        if data[i + 8 : i + 10] == b"\r\n":
            beacons.append((i, struct.unpack_from("<I", data, i + 4)[0], None, None))
        elif data[i + 16 : i + 18] == b"\r\n":
            beacons.append((i,) + struct.unpack_from("<III", data, i + 4))
    print(f"\nAPXB wrapper beacons: {len(beacons)} (each marks one payload launch)")
    for entry in beacons:
        i, reason = entry[0], entry[1]
        names = "|".join(n for b, n in RESETREAS_BITS if reason & b) or "none"
        line = f"  offset 0x{i:05X}  resetreas=0x{reason:08X} ({names})"
        if entry[2] is not None:
            gpregret2, marker = entry[2], entry[3]
            # These are the values this boot INHERITED, sampled before the
            # wrapper modified either. An armed cookie or a live marker here is
            # what makes the wrapper recover instead of launching again.
            line += (f"  gpregret2=0x{gpregret2:08X} (cookie=0x{gpregret2 & 0xFF:02X})"
                     f"  marker=0x{marker:08X}")
        print(line)
    if any(e[2] is not None for e in beacons):
        print("  note: a beacon means this boot LAUNCHED the payload, so the")
        print("        inherited cookie/marker shown were not recovery-triggering.")

    # Split by version. Stage 0 emits version 1, stage 1 version 2, and stage 2
    # emits version 3 on the replay launch or version 4 on a confirm launch.
    records, s1_records, s2_records, off = [], [], [], 0
    while True:
        i = data.find(APXG_MAGIC, off)
        if i < 0:
            break
        off = i + 1
        if i + 8 > len(data):
            continue
        version = struct.unpack_from("<H", data, i + 4)[0]
        if version == 1 and i + RECORD_SIZE <= len(data):
            records.append((i, dict(zip(FIELDS, struct.unpack_from(LAYOUT, data, i)))))
        elif version == 2 and i + S1_SIZE <= len(data):
            s1_records.append(i)
        elif version == 3 and i + S2_REPLAY_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 4 and i + S2_CONFIRM_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 5 and i + S2_REPLAY2_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 6 and i + S3_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 11 and i + S3_DIAG_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 12 and i + S3_USB_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 13 and i + S3_USBD_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 14 and i + S3_KICK_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 15 and i + S3_IRQ_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 16 and i + S3_EN_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 17 and i + S3_PRE_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 18 and i + S3_FORCE_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 19 and i + S3_NORM_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 32 and i + S3_TRAVEL_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 37 and i + S3_KBDCAP_SIZE <= len(data):
            s2_records.append((i, version))
        elif version in (38, 39) and i + S3_KBDCAP_SIZE <= len(data):
            # Pin survey. Identical 1280-byte layout to 37; only the kbd_cap_*
            # region is read differently. See G4B_EVIDENCE_VERSION_S3_PROBE.
            s2_records.append((i, version))
        elif version == 36 and i + S3_SPIM2_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 35 and i + S3_AIN_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 34 and i + S3_START_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 7 and i + S4_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 9 and i + S6_SIZE <= len(data):
            s2_records.append((i, version))
        elif version == 10 and i + S7_SIZE <= len(data):
            s2_records.append((i, version))

    if s2_records:
        stage7 = [r for r in s2_records if r[1] == 10]
        if stage7:
            print(f"\nAPXG stage-7 records: {len(stage7)}")
            for i, _v in stage7:
                print_s7(data, i)
            return 0
        stage6 = [r for r in s2_records if r[1] == 9]
        if stage6:
            print(f"\nAPXG stage-6 records: {len(stage6)}")
            for i, _v in stage6:
                print_s6(data, i)
            return 0
        stage4 = [r for r in s2_records if r[1] == 7]
        if stage4:
            print(f"\nAPXG stage-4 records: {len(stage4)}")
            for i, _version in stage4:
                print_s4(data, i)
            return 0
        stage3 = [r for r in s2_records if r[1] in (6, 11, 12, 13, 14, 15, 16, 17, 18, 19, 32, 34, 35, 36, 37, 38, 39)]
        if stage3:
            print(f"\nAPXG stage-3 records: {len(stage3)}")
            for i, _version in stage3:
                print_s3(data, i, _version)
            return 0
        print(f"\nAPXG stage-2 records: {len(s2_records)}")
        for i, version in s2_records:
            print_s2(data, i, version)
        return 0

    if s1_records:
        print(f"\nAPXG stage-1 records (version 2): {len(s1_records)}")
        for i in s1_records:
            head = struct.unpack_from(S1_HEAD, data, i)
            (_magic, _ver, stage, launch_ms, last_error, bringup_ok,
             enable_to_ready, op0, op1, _p0, _p1) = head
            print(f"\n--- stage-1 record at 0x{i:05X} ---")
            print(f"  stage={stage} launch_ms={launch_ms} bringup_ok={bringup_ok}")
            print(f"  last_error={last_error} ({ERRORS.get(last_error,'?')})")
            print(f"  enable->ready = {enable_to_ready} cycles ({enable_to_ready/64.0:.1f} us)")
            base = i + struct.calcsize(S1_HEAD)
            for n, opcode in enumerate((op0, op1)):
                rx, txa, rxa, ev, res, wtx, wrx = struct.unpack_from(
                    S1_EXCH, data, base + n * struct.calcsize(S1_EXCH))
                print(f"\n  exchange {n}: TX opcode 0x{opcode:02X}")
                print(f"    result={res} ({SPIM_RESULT.get(res,'?')})  "
                      f"tx_amount={txa} rx_amount={rxa} events_end={ev}")
                print(f"    ready wait: tx={wtx} us  rx={wrx} us")
                print(f"    rx[0:16] = {rx[:16].hex(' ')}")
                printable = "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in rx[:16])
                print(f"    rx ascii = |{printable}|")
                if opcode == 0x90:
                    oracle = rx[:6] == b"3.24.1"
                    print(f"    >>> ORACLE: rx[0:6] == '3.24.1' -> "
                          f"{'PASS - the link is correct end to end' if oracle else 'FAIL'}")
                    if not oracle and any(rx):
                        print("        non-zero but wrong: bit order, mode or alignment")
                    elif not any(rx):
                        print("        all zero: MISO not connected, or slave not driving")
                elif opcode == 0xA0:
                    if rx[0] == 0xA0:
                        print(f"    >>> A0 echo present; rx[1]={rx[1]} "
                              f"({'STM32 already configured' if rx[1] == 1 else 'NOT configured'})")
                    else:
                        print("    >>> A0 echo ABSENT (expected 0xA0 at rx[0])")
        return 0

    print(f"\nAPXG stage records: {len(records)}")
    if not records:
        print("  NONE. Either the payload never reached the stage thread, or the")
        print("  UART path failed. These are not the same thing - check the APXB")
        print("  count above: beacons present but no APXG means the wrapper ran")
        print("  and the payload did not get as far as emitting.")
        return 1

    seen = set()
    for i, r in records:
        key = tuple(sorted(r.items()))
        dup = key in seen
        seen.add(key)
        print(f"\n--- record at 0x{i:05X}{'  (duplicate copy)' if dup else ''} ---")
        print(f"  version={r['version']} stage={r['stage']} launch_ms={r['launch_ms']}")
        print(f"  last_error={r['last_error']} ({ERRORS.get(r['last_error'], '?')})")
        print(f"  bringup_ok={r['bringup_ok']}")
        if dup:
            continue

        print(f"\n  P0.IN   before=0x{r['p0_in_before']:08X}  after=0x{r['p0_in_after']:08X}")
        print(f"  P0.DIR  before=0x{r['p0_dir_before']:08X}  after=0x{r['p0_dir_after']:08X}")
        print(f"  P1.IN   before=0x{r['p1_in_before']:08X}  after=0x{r['p1_in_after']:08X}")
        print(f"  P1.DIR  before=0x{r['p1_dir_before']:08X}  after=0x{r['p1_dir_after']:08X}")

        print("\n  pull test (the decisive stage-0 measurement):")
        print(f"    P0.05 ready  before enable : {pull_verdict(r['ready_pullup_before'], r['ready_pulldown_before'])}")
        print(f"    P0.05 ready  after  enable : {pull_verdict(r['ready_pullup_after'], r['ready_pulldown_after'])}")
        print(f"    P0.24 attn   before enable : {pull_verdict(r['attn_pullup_before'], r['attn_pulldown_before'])}")
        print(f"    P0.24 attn   after  enable : {pull_verdict(r['attn_pullup_after'], r['attn_pulldown_after'])}")

        ready_woke = (r["ready_pullup_before"] != r["ready_pullup_after"]
                      or r["ready_pulldown_before"] != r["ready_pulldown_after"])
        print(f"    -> ready line changed character across the enable: {'YES' if ready_woke else 'no'}")

        us = r["dwt_enable_to_ready"] / (CPU_HZ / 1_000_000)
        win = r["dwt_observe_window"] / (CPU_HZ / 1_000_000)
        print(f"\n  timing: enable->first ready edge = {r['dwt_enable_to_ready']} cycles ({us:.1f} us)")
        print(f"          observe window            = {r['dwt_observe_window']} cycles ({win/1000:.1f} ms)")
        print(f"          ready edges counted       = {r['ready_edges']}")

        print(f"\n  GPIOTE.CONFIG[0]=0x{r['gpiote_config0']:08X} (expect 0x00010501)")
        print(f"  PIN_CNF ready=0x{r['pin_cnf_ready']:02X} attn=0x{r['pin_cnf_attn']:02X} miso=0x{r['pin_cnf_miso']:02X}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
