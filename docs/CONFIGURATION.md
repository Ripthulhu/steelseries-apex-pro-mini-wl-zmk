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
| `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` | `60` | Seconds between battery updates |
| `CONFIG_APEX_G4B_CHARGE_STOP_PCT` | `80` | Stop-charging threshold |
| `CONFIG_APEX_G4B_CHARGE_RESUME_PCT` | `72` | Resume-charging threshold |
| `CONFIG_APEX_G4B_GAMEPAD` | `y` | W/A/S/D analog gamepad over USB, or through the dongle in radio builds |

The scanner period is stored in one byte, so 255 ms is its limit. These slower
periods apply only on battery in Bluetooth mode. USB keeps the scanner at full
speed, and a key immediately returns it to full speed.

## Release logging

Release firmware compiles Zephyr logging out with `CONFIG_LOG=n`. The shell,
boot banner, `printk`, diagnostic USB/UART output, and standalone
capture tests are also off. This avoids serial traffic, extra startup work, and
needless wakeups on battery. The release builder rejects local overrides that
enable one of those test features.

`CONFIG_APEX_G4B_COREDUMP=y` is deliberately left on. It writes a small record
to external flash only after a fatal fault, then resets so A/B recovery can do
its job. It does not stream logs or run in the background.

## Radio diagnostics

The separate keyboard and receiver radio builds enable
`CONFIG_APEX_RADIO_SHELL=y`. This allows `keyboard battery`, `keyboard radio`
and other keyboard commands in the
receiver's serial terminal to run a command on the keyboard. It requires the
shell and radio-input support on both devices; it is not enabled in the normal
keyboard release. See [the receiver guide](../dongle/README.md#keyboard-diagnostics-through-the-receiver).

On the keyboard this adds a shell thread and fixed-size transfer buffers,
using about 5.8 KiB more RAM than the same radio build without it. The Bluetooth
shell remains disabled. Set `CONFIG_APEX_RADIO_SHELL=n` in a local override on
both devices to remove the remote shell without removing their USB consoles.

## USB data path

`CONFIG_APEX_G4B_USB_DATA_VBUS_GATE` is **on by default**. It drives the U10 USB
data switch from VBUS detection: the data pair is connected (P0.25 high) whenever
the port carries voltage and isolated (P0.25 low) on battery. A cable therefore
always powers the data path, so USB enumeration and the USB CDC shell, DFU, and
Studio endpoints are reachable whenever the keyboard is plugged, whether USB or
Bluetooth is the active output. The register-level trace of U10 is in
[reverse-engineering/USB_DATA_PATH.md](reverse-engineering/USB_DATA_PATH.md).

The pin boots high and the gate only drops it on confirmed battery operation, so
it never gates the first enumeration or DFU recovery. Turning it off holds P0.25
high at all times — the prior behavior, and harmless, it just leaves the data
switch powered on battery where there is no host to reach.

This is the safe half of the stock policy. Stock also cuts the data path in
charge-only mode (a cable in for power while the output is Bluetooth or the
dongle); this firmware does not, because that would drop the USB CDC debug
endpoints while charging in Bluetooth mode.

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

The generic flash shell uses `CONFIG_FLASH_SHELL_BUFFER_SIZE=0x400` (1 KiB).
It allocates two buffers, so the former 4 KiB setting consumed 8 KiB of RAM.
Read/test commands using those buffers now accept at most 1 KiB per operation.
Use smaller consecutive reads for a larger range, or `flash load` for streamed
writes. The keyboard's update, recovery and settings drivers don't use these
shell buffers. You can override the value in a diagnostic build if needed.

The external NOR driver also uses a separate 1 KiB read buffer. Larger requests
are split into consecutive SPI reads. This doesn't change flash addresses,
record formats or the amount a caller can read, but adds transactions to bulk
reads. Its buffer no longer reserves space for an entire 4 KiB dump record.

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
- `CONFIG_APEX_G4B_DONGLE_RADIO`. This releases the Bluetooth controller for
  radio development; it does not enable a working dongle connection.

If Kconfig warns that a value was ignored, do not force it elsewhere. Some
symbols are selected by another feature or calculated by Zephyr. Change the
user-facing option that controls it instead.
