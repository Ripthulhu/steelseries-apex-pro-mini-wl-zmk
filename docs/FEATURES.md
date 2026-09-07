# Features

This page covers the board-specific features added to ZMK. The shared flash
layout is in [FLASH_MEMORY_MAP.md](FLASH_MEMORY_MAP.md).

## Storage

- **ZMK's non-volatile settings store (NVS)** stores bonds, RGB state, and
  settings in external NOR behind a provision marker, so stale vendor data
  cannot be mistaken for a valid store. NVS writes are `g4b_extbus`-locked and
  held until scanner boot replay finishes.
- Space remains reserved for an experimental LittleFS mount, which is disabled
  because the firmware has no use for it.

## Coredumps

`CONFIG_APEX_G4B_COREDUMP` handles a fatal fault by writing the CPU state to a
16 KiB ring in external NOR, then resetting. This is part of recovery,
not continuous logging. Open `APEXBOOT` to see the newest record summarized in
`INFO_UF2.TXT`; its validated 248-byte record is also available as `CRASH.BIN`.
The path was tested with SWD fault injection, and the file exported by the
bootloader was checked against its stored CRC.

## A/B recovery

Every release keeps a recovery copy of the application in external flash. The
application records each boot and clears the count after an uninterrupted run
of scanner replies. If USB is the selected or required output, its HID endpoint
must also be ready. A charger or battery bank in a wireless switch position
does not require USB enumeration for the health check. On the first healthy boot after an
update, the firmware copies the running image to the 452 KiB recovery slot in
external NOR.

Fatal CPU faults reset immediately. The Nordic hardware watchdog resets a
complete firmware freeze within 60 seconds; its configuration and scanner-stall
timing are described in [Configuration](CONFIGURATION.md#watchdog).

After three recorded unhealthy boots, the bootloader checks the saved image's
layout and CRC, copies it back to internal flash, and clears its application
metadata. The descriptor and boot tally use separate sectors.

This was tested with a 335,612-byte application and a build that deliberately
failed during startup. After the third failure, the restored application
matched the saved image byte for byte, UICR was unchanged, and the healthy
application staged itself again. The bootloader implementation is in
[`ab_promote.c`](../bootloader/apex_pro_mini_wl/ab_promote.c).

## Lighting effects

Fn+X cycles the standard ZMK lighting mode plus eleven custom effects at up to
200 Hz. The choice persists with the other keyboard settings, and every effect
obeys the same 0–100% brightness control and hard ceiling.

**Shockwave** launches a coloured ring from the position of each pressed key.
**Digital Rain** runs several independent moving trails, while **Prismatic Ink**
diffuses each key-down into neighbouring keys. Analog and Heat Map remain the
only effects that request Hall depth samples.

## Analog gamepad

The optional gamepad maps Hall depth from W/A/S/D to five DirectInput axes:
D−A steering on X and Rx, S−W on Y, W throttle on Z, and S brake on Rz. It is
toggled with `Fn`+`Z` and is not exposed over Bluetooth.

Matching radio builds also forward these axes through the dongle. Direct keyboard
USB takes priority when connected to a computer. While controller output is
enabled and connected, the scanner stays at its active cadence and the keyboard
does not enter deep sleep. Switch it off with `Fn`+`Z` to restore normal idle
power savings. RGB still follows its usual timeout.

Game support varies. Axis assignments may need to be remapped per game, and
games that reject simultaneous keyboard and controller input may need a mod or
may not work well with this mode at all.

The scanner returns all four samples in one `0xA2` response. Adjacent keys had
different measured travel ranges, so the firmware learns rest and bottom-out
per key instead of sharing fixed limits. Gradual axis movement has been tested
on Windows through both direct USB and the dongle, with normal typing available.

## Battery charge limit

The BQ25895 charge controller is configured through an allowlist of writable
registers:

- **`CONFIG_APEX_G4B_CHARGE_LIMIT`** (default on): caps charge voltage at **4.096 V**
  instead of the pack's 4.400 V rating, reducing the time the cell spends near
  its highest voltage. Charge current is limited to **1,472 mA**, about **0.25C**
  for the 5,870 mAh pack. The runtime reduction has not been measured.
- **`CONFIG_APEX_G4B_CHARGE_STORAGE`** (default on): a charge hold band.
  The BQ25895's power path runs the keyboard from USB regardless (it runs with the
  battery physically removed), so when the pack reaches the stop point the
  controller clears **REG03 `CHG_CONFIG`** to stop charging while USB powers the
  keyboard, and resumes at the lower threshold. Defaults:
  stop **80 %**, resume **72 %** on the stock firmware's voltage lookup table.
  These are estimates, not measured percentages of remaining capacity. The gap
  avoids restarting charge immediately as voltage settles. Charge state is
  checked during ZMK's existing battery update, without another thread. The host
  reports 100 % at the capped full because `APEX_G4B_BATT_FULL_MV` matches the
  cap.

The pack is rated 3.85 V nominal, 5,870 mAh (22.5995 Wh), with a 4.4 V maximum
charge voltage. The `80` charge preset selects the conservative 4.096 V ceiling;
it does not guarantee exactly 80% state of charge. The board has no coulomb
counter, and the stock voltage table has not been calibrated against measured
capacity here. Lower-voltage charging does not guarantee freedom from swelling.

## Wireless power

The wireless power controls have different USB conditions:

- In Bluetooth or dongle mode with no VBUS, the scanner changes from its 1 ms active
  period to 50 ms after five quiet seconds, then to 255 ms after one minute. The
  period field is one byte, so 255 ms is its maximum value. P0.24 ATTN wakes the
  Nordic on a key change and the scanner returns to 1 ms. Physical key wake has
  been tested from both idle periods. Current draw and first-key latency have
  not been measured.
- The Nordic link thread sleeps between ATTN, Bluetooth, and housekeeping work.
  Its 200 ms housekeeping timeout does not delay the ATTN interrupt.
- With no VBUS, sustained pressure across several keys is treated as a keyboard
  packed in a bag. The firmware suppresses the held keys, blanks RGB, and polls
  at 250 ms until the pressure is removed. The thresholds are listed in
  [Configuration](CONFIGURATION.md#useful-settings).
- In either wireless mode without an active USB HID connection, RGB starts fading
  after 30 seconds and then switches off both LED rails. SPIM2 and its pins are
  parked while the rails are off and restored before the controller is
  initialized on wake. A charge-only battery bank does not prevent this timeout.
- A configured USB HID host takes over input even when the switch is in the
  Bluetooth position. Unplugging it returns input to Bluetooth. VBUS from a
  charger or battery bank does not select USB without a working data connection.
- Battery-only startup leaves USBD and its high-frequency crystal request off
  until VBUS appears.
- The BQ25895 ADC runs on demand at the battery-report interval instead of
  converting continuously.
- The optional analog gamepad stops requesting `0xA2` depth samples when it is
  off or neither USB power nor a selected radio connection is available.

The nRF uses its DC/DC converter. Bluetooth requests a 7.5–15 ms connection
interval with peripheral latency 30; the host chooses the final values.

nRF System OFF uses `CONFIG_APEX_G4B_SLEEP_MS` (15 minutes in the current build).
Dongle mode additionally requires `CONFIG_APEX_G4B_DONGLE_SLEEP`, enabled by the
radio-input configuration. Wake reboots the keyboard and reconnects; it is not
instant and the initial tap may be lost. Key and switch wake in dongle mode passed
tests with a 60-second timeout; the full 15-minute wait and sleep current have
not been measured. From dongle sleep, move the switch fully to Bluetooth to wake
without a key; the middle USB position is not a guaranteed wake level.

STM32 STOP1 remains disabled: mode 0
stops the scanner, and neither a key nor the reconstructed link wake sequence
restored it.
