# Adafruit nRF52 bootloader board port

This directory adapts the
[Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader)
to the first-generation SteelSeries Apex Pro Mini Wireless. The current
application is SoftDevice-free and starts at `0x1000`.

For the installed flash map and DFU entry methods, see
[docs/BOOTLOADER.md](../docs/BOOTLOADER.md). For the initial SWD install, see
[docs/INSTALL.md](../docs/INSTALL.md). For repair after installation, see
[docs/SWD_RECOVERY.md](../docs/SWD_RECOVERY.md).

## Contents

| Path | Purpose |
|---|---|
| `apex_pro_mini_wl/board.h` | nRF52833 board, USB IDs, UF2 family, and app floor |
| `apex_pro_mini_wl/pinconfig.c` | USB rail setup and the RGB DFU indicator |
| `apex_pro_mini_wl/ab_promote.c` | Validated A/B recovery from external NOR |
| `apex_pro_mini_wl/nrf52833_apex_legacy.ld` | Factory MBR-compatible migration layout |
| `patches/` | Changes applied to the upstream bootloader tree |

The bootloader raises P0.25 to enable the U10 USB data path, then raises P0.23
and P0.19 for the RGB matrix used as its status display. Without P0.25,
`APEXBOOT` cannot enumerate.

In DFU, the key matrix is green while idle and red while writing. The volume
label is `APEXBOOT`, the USB ID is `1D50:616F`, and the application UF2 family
is `0x621E937A`.

## Local changes

The patches make eight targeted changes:

1. enforce the board-defined application floor for app-family UF2 writes;
2. connect bootloader state to the board's RGB DFU indicator;
3. use a caller-supplied Zephyr SDK toolchain path;
4. mask interrupts immediately before jumping to the application;
5. restore a validated A/B-v2 image after three unhealthy boots;
6. provide an opt-in layout matching the factory MBR's fixed addresses; and
7. use the locked source date for reproducible bootloader version metadata; and
8. add the reset cause, A/B state, and newest crash summary to `INFO_UF2.TXT`,
   with the validated raw record available as `CRASH.BIN`.

The A/B restore accepts only descriptor version 2, app base `0x1000`, image-B
offset `0x8B000`, and a length no larger than `0x71000`. It verifies the staged
CRC before erasing internal flash and verifies the restored image afterward.
The factory-compatible build limits images to `0x66000` bytes so rollback cannot
reach its bootloader at `0x6E000`.

## Building

From the repository root, the recommended release command creates a pinned,
isolated bootloader checkout, applies this directory's board port and patches,
and builds it together with the application:

```sh
python tools/build_release.py
```

The builder checks out the tested upstream revision, applies every locked patch
in order, and uses the same pinned compiler as the application. This avoids a
second manual recipe that can silently drift from the release build.

The experimental USB installer uses a factory-compatible loader built with
`python tools/build_release.py --installer-bootloader`. It links at `0x6E000`,
uses the MBR parameter page at `0x6B000`, keeps settings at `0x6C000`, and ends
at `0x78000`. It is not part of the normal release bundle; see
[`../installer/`](../installer/) for the complete build and test procedure.

The complete first-install bundle is written to
`../work/release/apex-pro-mini-wl-ab.zip`. Its `bootloader_mbr.hex` contains the
MBR and bootloader used by the SWD installation procedure. The adjacent
`apex-pro-mini-wl-ab.uf2` is the application-only file for normal updates.

## Installing safely

The normal `0x74000` build's `bootloader.hex` contains UICR records. Programming
it naively can erase the whole UICR page, including reset-pin, NFC/GPIO, and
APPROTECT settings. For an update to that layout, either flash only
`0x74000..0x7E000` or restore and verify the complete UICR backup immediately
afterward. The separate `0x6E000` migration build leaves factory UICR untouched.

The tested first-installation method uses SWD. The no-SWD migration in
[`../installer/`](../installer/) has not been tested on a stock keyboard. Once a
bootloader is present, normal application updates use UF2 or serial DFU. Follow
the complete repair sequence in
[docs/SWD_RECOVERY.md](../docs/SWD_RECOVERY.md).
There is no S140 SoftDevice in the installed layout.

The matching full-size A/B-v2 bootloader and application were tested by staging
a recovery image, forcing three failed boots, then verifying byte-exact rollback,
UICR preservation, and automatic restaging. The current behavior is described in
[docs/FEATURES.md](../docs/FEATURES.md#ab-recovery).

The bootloader-only update was also tested over SWD. `INFO_UF2.TXT` reported the
saved A/B state and newest crash, `CRASH.BIN` matched its stored CRC, and two
quick pulses on the physical reset line opened `APEXBOOT`.
