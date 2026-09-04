# Configuration

The downloaded firmware uses the settings in this repository. You do not need
to edit anything to install it.

ZMK and Zephyr use Kconfig files: one `CONFIG_NAME=value` setting per line.
The main release settings are in [`g4b_usb.conf`](../apex-zmk-g4b/g4b_usb.conf).
Scanner idle timing is kept in
[`g4b_wireless_idle.conf`](../apex-zmk-g4b/g4b_wireless_idle.conf), and
[`g4b_ab_v2.conf`](../apex-zmk-g4b/g4b_ab_v2.conf) enables recovery for every
release build.

## Make a local change

Keep personal settings in a separate file so updating the repository does not
overwrite them. For example, create `my-keyboard.conf`:

```ini
CONFIG_ZMK_KEYBOARD_NAME="My Apex"
CONFIG_APEX_G4B_RGB_IDLE_MS=60000
CONFIG_APEX_G4B_RAPID_TRIGGER=2
```

Build it with:

```sh
python tools/build_release.py --extra-conf my-keyboard.conf
```

The extra file is read last, so it overrides the normal setting. The build still
enables and checks A/B recovery. The finished bundle includes
`apex-zmk.config`, which records the final value of every Kconfig option.

## Useful settings

| Setting | Release value | Meaning |
|---|---:|---|
| `CONFIG_ZMK_KEYBOARD_NAME` | `"Apex Pro Mini WL"` | USB and Bluetooth device name |
| `CONFIG_APEX_G4B_RGB_IDLE_MS` | `30000` | Begin fading RGB after this much Bluetooth idle time; the rail turns off about 10 seconds later, and `0` disables the timeout |
| `CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX` | `100` | Maximum RGB brightness in percent |
| `CONFIG_APEX_G4B_RAPID_TRIGGER` | `3` | Rapid-trigger travel in tenths of a millimetre; `0` disables it |
| `CONFIG_APEX_G4B_STM32_IDLE_SCAN_PERIOD_MS` | `50` | Scanner period after five idle seconds on Bluetooth |
| `CONFIG_APEX_G4B_STM32_LONG_IDLE_SCAN_PERIOD_MS` | `255` | Scanner period after one idle minute on Bluetooth |
| `CONFIG_APEX_G4B_STM32_LONG_IDLE_AFTER_MS` | `60000` | Time before the second scanner idle tier |
| `CONFIG_APEX_G4B_BAG_GUARD` | `y` | Suppress sustained multi-key pressure while on battery |
| `CONFIG_APEX_G4B_BAG_KEYS` | `4` | Held-key count that starts the eight-second guard timer |
| `CONFIG_APEX_G4B_BAG_IDLE_MS` | `8000` | Time before four to seven held keys are treated as bag pressure |
| `CONFIG_APEX_G4B_BAG_POLL_MS` | `250` | Scanner period while the guard is active |
| `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` | `60` | Seconds between battery updates |
| `CONFIG_APEX_G4B_CHARGE_STOP_PCT` | `80` | Stop-charging threshold |
| `CONFIG_APEX_G4B_CHARGE_RESUME_PCT` | `72` | Resume-charging threshold |
| `CONFIG_APEX_G4B_GAMEPAD` | `y` | USB analog gamepad interface for W/A/S/D |

The scanner period is stored in one byte, so 255 ms is its limit. These slower
periods apply only on battery in Bluetooth mode. USB keeps the scanner at full
speed, and a key immediately returns it to full speed.

The bag guard handles sustained pressure rather than ordinary chords. Four to
seven held keys engage it after eight seconds; eight or more engage it after one
second. It turns RGB off, suppresses those key reports, and returns to normal
once no more than two keys remain held. It is disabled whenever USB power is
present.

## Release logging

Release firmware compiles Zephyr logging out with `CONFIG_LOG=n`. The shell,
boot banner, `printk`, diagnostic USB/UART output, and standalone
capture tests are also off. This avoids serial traffic, extra startup work, and
needless wakeups on battery. The release builder rejects local overrides that
enable one of those test features.

`CONFIG_APEX_G4B_COREDUMP=y` is deliberately left on. It writes a small record
to external flash only after a fatal fault, then resets so A/B recovery can do
its job. It does not stream logs or run in the background.

## Watchdog

`CONFIG_APEX_G4B_WATCHDOG=y` starts the nRF52833 hardware watchdog with a
60-second timeout. A complete firmware freeze prevents further watchdog feeds,
so the Nordic resets within 60 seconds. A scanner-loop stall is first detected
after 30 seconds; the feed then stops, so that case resets within about 90
seconds.

The watchdog keeps running across software resets. The bundled Adafruit
bootloader already feeds inherited watchdog channels while its UF2 and serial
DFU loop is active, so entering `APEXBOOT` does not put an update on a timer.
Leave this setting enabled in normal builds.

The numbered `.conf` files in `apex-zmk-g4b` are hardware-diagnostic builds. Do
not turn on `CONFIG_ZMK_USB_LOGGING` in a normal build: upstream ZMK's USB logger
selects the legacy USB stack, while this keyboard uses Zephyr's current USB
device stack.

## Settings to leave alone

Do not override the following in a normal local build:

- `CONFIG_APEX_G4B_AB_ROLLBACK`, `CONFIG_APEX_G4B_AB_AUTOSTAGE`, or the flash
  layout. The application and bootloader must agree on the recovery slot.
- `CONFIG_APEX_G4B_AB_CRASHTEST`. It intentionally crashes every new boot and is
  used only for the attended recovery test described in
  [Building from source](BUILDING.md#testing-ab-recovery).
- `CONFIG_USB_DEVICE_STACK_NEXT` or the legacy USB-stack settings.
- `CONFIG_SPI`, `CONFIG_GPIO`, or `CONFIG_PINCTRL`. This board's scanner, RGB,
  flash, and charger drivers own their peripherals directly.
- `CONFIG_ARM_MPU`, `CONFIG_HW_STACK_PROTECTION`, `CONFIG_SRAM_SIZE`, or the
  application partition. They are tied to the board's custom memory layout.
- `CONFIG_APEX_G4B_DONGLE_RADIO` or `CONFIG_APEX_G4B_ESB`. The 2.4 GHz dongle
  protocol is unfinished and is not part of release firmware.

If Kconfig warns that a value was ignored, do not force it elsewhere. Some
symbols are selected by another feature or calculated by Zephyr. Change the
user-facing option that controls it instead.
