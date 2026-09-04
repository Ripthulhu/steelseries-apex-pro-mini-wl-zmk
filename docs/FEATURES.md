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
not continuous logging. Release builds do not print the record; a diagnostic
build can read it later. The path was tested with SWD fault injection.

## A/B recovery

Every release keeps a recovery copy of the application in external flash. The
application records each boot and clears the count after an uninterrupted run
of scanner replies. If USB is the selected or required output, its HID endpoint
must also be ready. A charger or battery bank in Bluetooth mode leaves BLE
selected and does not block the health check. On the first healthy boot after an
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

The optional USB gamepad maps Hall depth from W/A/S/D to five DirectInput axes:
D−A steering on X and Rx, S−W on Y, W throttle on Z, and S brake on Rz. It is
toggled with `Fn`+`Z` and is not exposed over Bluetooth.

Game support varies. Axis assignments may need to be remapped per game, and
games that reject simultaneous keyboard and controller input may need a mod or
may not work well with this mode at all.

The scanner returns all four samples in one `0xA2` response. Adjacent keys had
different measured travel ranges, so the firmware learns rest and bottom-out
per key instead of sharing fixed limits. The gamepad was tested on Windows with
continuous axis movement while the normal keyboard interface remained active.

## Battery charge limit

The BQ25895 charge controller is configured through an allowlist of writable
registers:

- **`CONFIG_APEX_G4B_CHARGE_LIMIT`** (default on): caps charge voltage at **4.096 V**
  instead of the pack's 4.400 V rating, reducing the time the cell spends near
  its highest voltage. This costs roughly a quarter of its rated runtime.
- **`CONFIG_APEX_G4B_CHARGE_STORAGE`** (default on): a charge hold band.
  The BQ25895's power path runs the keyboard from USB regardless (it runs with the
  battery physically removed), so when the pack reaches the stop point the
  controller clears **REG03 `CHG_CONFIG`** — the cell is left idle, neither charged
  nor discharged — and resumes only when it sags to the resume point. Defaults:
  stop **80 %**, resume **72 %** (of the 4.4 V-referenced curve; the ~8-point gap
  accounts for the resting-voltage settle after a charge). Charge state is
  checked during ZMK's existing battery update, without another thread. The host
  reports 100 % at the capped full because `APEX_G4B_BATT_FULL_MV` matches the
  cap.

## Wireless power

The wireless power controls have different USB conditions:

- With Bluetooth selected and no VBUS, the scanner changes from its 1 ms active
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
- In Bluetooth mode without an active USB HID connection, RGB starts fading
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
- The optional analog gamepad stops requesting `0xA2` depth samples when USB is
  absent.

The nRF uses its DC/DC converter. Bluetooth requests a 7.5–15 ms connection
interval with peripheral latency 30; the host chooses the final values.

nRF System OFF is disabled because wake requires a reset and Bluetooth took
about 16 seconds to reconnect in testing. STM32 STOP1 is also disabled: mode 0
stops the scanner, and neither a key nor the reconstructed link wake sequence
restored it.
