# USB installer for stock first-generation keyboards — untested

This targets the first-generation Apex Pro Mini Wireless and installs the
Adafruit-based `APEXBOOT` bootloader through the stock SteelSeries USB update
protocol. It is untested on an untouched keyboard. Only try it after confirming
that your SWD programmer can talk to the keyboard. If it fails, use SWD to erase
the Nordic and install the open firmware.

The factory bootloader cannot be dumped from a stock keyboard. APPROTECT blocks
SWD reads, disabling it erases the chip, and the available SteelSeries updater
files do not contain the bootloader. Without that dump, a stock keyboard cannot
be recreated to test the whole USB path. Offline validation passes, and the MBR
copy was exercised by starting the migration through SWD. The complete path
from the SteelSeries USB updater to `APEXBOOT` remains untested.
The tested first-installation path uses SWD as described in
[`docs/INSTALL.md`](../docs/INSTALL.md).

The migration keeps Nordic's Master Boot Record (MBR) at `0x0000`. The MBR is
separate from the SteelSeries bootloader being replaced. A migration application
arrives through the normal SteelSeries update channel, verifies an embedded
bootloader, and asks the MBR to copy it to the factory bootloader address. The
new bootloader then stays in `APEXBOOT`, ready for the regular application UF2.

## Test status

- Image layout, vectors, CRCs, and rejected-input cases pass the offline tests.
- Starting the migration through SWD installed the bootloader and opened
  `APEXBOOT` on the real nRF52833.
- Resetting during the MBR copy resumed the journaled operation successfully.
- Installing the first application UF2 from `APEXBOOT` worked.
- Sending the migration through the stock normal and recovery USB interfaces has
  not been tested.

## Flash layout during migration

| Range | Purpose |
|---|---|
| `0x00000..0x1C000` | Factory MBR + Nordic S113 Bluetooth stack; retained during stage one |
| `0x1C000..0x5B000` | Migration application |
| `0x5B000..0x5C000` | Layout metadata and CRC |
| `0x5C000..0x66000` | New 40 KiB bootloader source image |
| `0x66FFC` | SteelSeries image CRC trailer |
| `0x6B000` | Factory MBR parameter page |
| `0x6C000` | New bootloader settings page |
| `0x6E000..0x78000` | New UF2 bootloader destination |

The MBR copy moves only the contiguous loader payload. On its first start, the
new loader erases the factory settings record at `0x6C000`. It repeats that erase
while S113 remains and stays in update mode until the first complete UF2 has
replaced S113 and passed application validation.

The final firmware remains linked at `0x1000`. In this compatibility layout its
maximum accepted image is `0x66000` bytes (408 KiB), ending at `0x67000`. The
current release is below this limit, stores settings in external NOR, and
retains A/B rollback with the same 408 KiB ceiling.

## Checks before writing

- The migration checks the factory MBR footer words (`0xFF8 = 0x6E000`,
  `0xFFC = 0x6B000`) before changing anything.
- It verifies the embedded bootloader vectors and CRC before invoking the MBR.
- Packaging rejects populated internal-flash addresses outside the selected
  legacy loader, settings, and parameter-page regions instead of truncating an
  oversized loader. It also requires the loader HEX's UICR markers to name the
  factory addresses, although the migration deliberately leaves the keyboard's
  UICR untouched because the MBR footer has priority.
- It does not erase the MBR, rewrite S113 directly, or modify UICR.
- While S113 remains, the new loader stays in DFU. It also disables the generic
  vector-only fallback, so an interrupted first UF2 remains recoverable instead
  of launching a partial image.
- A validation or MBR-call failure invalidates only the temporary application,
  causing the still-present factory bootloader to return to recovery on the
  next reset.
- The host tool validates the full SHA-256, payload metadata, both CRCs, vectors,
  exact USB PID, and device-reported staged-image CRC before it sends reset.

## Build the migration image

The normal release does not include this migration image while the USB path is
untested. From the repository root, prepare the pinned toolchain, build the
factory-layout bootloader, and package it with the migration application:

If your Python command is named `python3`, use that name below.

```sh
python tools/setup_workspace.py
python tools/build_release.py --installer-bootloader
python installer/build_migration.py ../work/installer/bootloader-legacy.hex
```

The script produces `installer/artifacts/apex-stock-to-uf2.vendor.bin` and
prints its SHA-256. It validates the finished image but does not access the
keyboard. If you use `--work-root` during setup, pass the same option to both
build commands and use that directory's `installer/bootloader-legacy.hex`.

Release files and local `tools/build_release.py` output are already checked
against the factory-compatible ceiling. Run the same check manually for an
application built another way:

```sh
python installer/verify_final_uf2.py path/to/application.uf2
```

To recheck an existing migration image without USB access:

```sh
python installer/flash_stock.py installer/artifacts/apex-stock-to-uf2.vendor.bin --confirm-sha256 <printed-sha256>
```

## Send it to the keyboard

Have the current `apex-pro-mini-wl-ab.uf2` from the
[latest release](https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk/releases/latest)
ready before starting.

Install the HID library, then add `--flash` and choose the stock USB channel:

```sh
python -m pip install -r requirements-host.txt
python installer/flash_stock.py installer/artifacts/apex-stock-to-uf2.vendor.bin --confirm-sha256 <printed-sha256> --normal --flash
```

Use `--recovery` instead when the keyboard is already at `1038:1627`.

On Linux, `hidapi` may need permission to open the two SteelSeries interfaces.
If the command reports a permission error, open
`/etc/udev/rules.d/70-apex-stock-installer.rules` as root and add:

```udev
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1038", ATTRS{idProduct}=="1626", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1038", ATTRS{idProduct}=="1627", TAG+="uaccess"
```

Reload the rules with `sudo udevadm control --reload-rules`, then disconnect and
reconnect the keyboard.

After the migration succeeds, `APEXBOOT` appears. Copy the current application
UF2 to it using the [normal update steps](../docs/FLASHING.md#normal-update).

## Test on an untouched keyboard

Testing requires an untouched keyboard with its protected factory loader still
present. Connect SWD and confirm the Nordic CTRL-AP responds, but do not unlock
it: unlocking performs a mass erase, and protected flash cannot be backed up.
Then send the migration through both stock USB IDs, install the first UF2 from
`APEXBOOT`, and verify USB, Bluetooth, scanner wake, and saved settings. For the
interruption tests, briefly short CN3 `RESET` to `GND` once during the MBR copy
and once during the first UF2 write, then confirm that `APEXBOOT` returns. If the
USB migration fails, use SWD to erase the Nordic and install the open firmware.

A post-conversion flash dump cannot stand in for the missing factory loader at
`0x6E000..0x78000`.
