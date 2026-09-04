# Firmware

The keyboard firmware is a ZMK application for the nRF52833. It's split across two
Zephyr modules:

- **[`apex-zmk-slot/`](../apex-zmk-slot/)** — the **board definition**: devicetree,
  pinctrl recovered from the stock image, the keymap, and board Kconfig.
- **[`apex-zmk-g4b/`](../apex-zmk-g4b/)** — the **application module**: the `src/`
  drivers that make this specific hardware work, the config, the devicetree
  bindings, and build/verification tools that do not require `west`.

It's built against a pinned Zephyr/ZMK tree and linked to run at **`0x1000`**,
above the MBR and below the [Adafruit bootloader](BOOTLOADER.md). The stock
keyboard's S113 and this port's earlier S140-based layout are both gone;
Bluetooth uses Zephyr's controller.

## Startup

The board-specific link work starts in `g4b_main` in `link_g4b.c`. It initializes
the STM32 link and enters the keyboard scan loop; diagnostic builds may run a
hardware test first. ZMK, Bluetooth, the watchdog, A/B staging, and crash-record
handling use their own Zephyr work items or threads.

A Nordic reset can leave the STM32 partway through its previous session. At
startup the firmware therefore holds both scanner enables low for 250 ms,
powers it again, waits for READY, and replays the 59 setup frames. UF2 updates
do not need a power cycle or a USB replug afterward.

Release builds enable scanner ingestion and USB Studio. Numbered build profiles
are retained only for individual hardware diagnostics.

## Key scanning

Keys come from the STM32, not from a matrix ZMK scans itself. `kscan_g4b.c` is a
ZMK kscan driver, but instead of driving rows and reading columns it takes a
bitmap handed to it by the link code and reports pressed/released positions to
ZMK's keymap. The link code polls the STM32's attention line, reads the `0xA1`
key report when it's high, and calls `apex_g4b_kscan_ingest_bitmap` with the
result. From ZMK's point of view it's an ordinary kscan device feeding an
ordinary keymap — the Fn layer, the RGB controls, all of it work normally.

The permanent scan loop deliberately mirrors what the stock poll task does: read
`0xA1` when attention is high, ingest on any successful read, and deduplicate
only against the previous bitmap.

The bitmap contains absolute key levels. Repeated reads are harmless, while
waiting for two identical reports can lose a key event because attention drops
after the first report is consumed.

The scanner link protocol — opcodes, the key report, actuation, and the flash
write to avoid — is documented in [PROTOCOL.md](PROTOCOL.md).

### How the scanner link was verified

The link was reconstructed by comparing the stock Nordic code, the STM32
application, and captures from a keyboard running the factory firmware. The
hardware tests then checked it in stages:

1. Powering the STM32 made READY assert after about 80.2 ms.
2. Opcode `0x90` returned `3.24.1`, confirming the pins, SPI mode, byte order,
   two-part transfer, and READY handshake together.
3. The firmware replayed all 59 captured setup frames; every 64-byte reply
   matched the corresponding factory capture.
4. An `0xA0` read reported the scanner configured immediately after that replay
   and unconfigured after a Nordic reset. The replay is therefore required on
   every boot rather than being a one-time setup step.

## The mode switch

`mode_g4b.c` reads the three-position slide switch on P0.03 (via the SAADC) and
classifies it as Bluetooth, USB-only, or dongle:

- **Bluetooth** — BLE advertises and can be selected, as stock.
- **USB-only** — BLE is held down; USB is the only endpoint.
- **Dongle** — reserved for the vendor 2.4 GHz receiver. The release does not
  implement this transport, so this position behaves like USB-only: a cable
  works, but the SteelSeries receiver does not.

The switch is a voltage on an ADC pin and is sampled before the transports start.
Experimental dongle builds reset when crossing the dongle boundary so BLE and
the ESB driver never own the radio at the same time. Release builds keep both
dongle options disabled. Use [`APEXBOOT` or SWD](FLASHING.md) for recovery.

## Lighting

ZMK sees the LEDs as an underglow device, but the board driver does not use
Zephyr's SPI or GPIO drivers. `rgb_g4b.c` drives the IS31FL3743B through direct
SPIM2 register writes, and `led_strip_g4b.c` implements Zephyr's `led_strip`
`update_rgb` API on top of it.

That indirection is deliberate. Going through Zephyr's SPI/LED stack would pull in
`CONFIG_SPI`, `CONFIG_PINCTRL`, and `CONFIG_GPIO`, all of which the verifier
constrains — pinctrl reconfigures pins at boot, and this firmware keeps every pin
write in one auditable place (`pins_g4b.c`, the sole owner of GPIO register
writes). `rgb_fx_g4b.c` is a higher-rate effect engine that runs alongside ZMK's
underglow, and `rgb_map_g4b.c` holds the LED↔key mapping tables recovered from
the stock image.

## Firmware updates and recovery

`behavior_dfu_g4b.c` handles the `Fn`+`Right Ctrl`+`Esc` combination, while
`dfu_trigger_g4b.c` handles the dedicated USB serial interface. Both write the
Adafruit bootloader magic directly to `GPREGRET`; `sys_reboot()` does not set it
in this tree. The implementation detail and complete entry-method table are in
[Bootloader](BOOTLOADER.md#entering-dfu), the installed layout is in
[Flash layout](FLASH_MEMORY_MAP.md), and the normal user procedure is in
[Updating and recovery](FLASHING.md).

`recovery_usb_g4b.c` and `recovery_g4b.c` are disabled prototypes for the
unavailable SteelSeries loader. They are not part of release firmware.

The application also provides:

- `spinor_g4b.c` / `flash_spinor_g4b.c` — the external FM25Q08A SPI-NOR (1 MiB,
  SPIM0, bit-banged CS P0.26), freed by the bootloader swap. `spinor_g4b.c` is
  the direct-register access (read/erase/page-program); `flash_spinor_g4b.c`
  wraps it as a Zephyr **flash device** (`apex,g4b-spinor`, gated on
  `CONFIG_APEX_G4B_SPINOR_FLASHDEV`) so ZMK settings/NVS live on the external
  chip. The custom driver avoids enabling generic pinctrl and GPIO ownership.
  Scanner and flash operations use `g4b_extbus_*`, writes wait for boot replay,
  and SPIM0 is enabled only for each operation. The buses do not share pins; the
  lock stops a settings write on another Zephyr work item from interrupting a
  two-part scanner transfer or the paced startup replay. Settings and bonds
  survive application updates and power cycles.
- `updater_g4b.c` — retained legacy source for the pre-swap SteelSeries loader.
  It is disabled in the current build and its HID node is absent because the
  Adafruit bootloader does not consume SteelSeries' staged SPI-NOR image.

## The radio

`radio_g4b.c` can stand the BLE controller down when the mode switch selects
dongle, and `radio_esb_g4b.c` contains the unfinished vendor-link experiment.
They are gated by `CONFIG_APEX_G4B_DONGLE_RADIO` and
`CONFIG_APEX_G4B_ESB`; both are disabled and omitted from release firmware.
Diagnostic builds can enable them for radio work. Pairing is not complete, and
the current transmit sweep is not suitable for normal battery use. The recovered
protocol and PHY details are in
[reverse-engineering/RADIO.md](reverse-engineering/RADIO.md).

## Build checks

`verify_g4b_plain.py` is the active Adafruit-image checker. It never builds or
flashes. Every `--plain-image` build runs it after generating `.bin`, `.hex`, and
`.uf2`; it checks application bounds, vectors and RAM, exact artifact bytes,
every UF2 block and family ID, release configuration, DC/DC mode, matching A/B
layouts in the application and bootloader, required USB patches, USB selection
on battery power, and RGB bus shutdown and restart order.

`verify_g4b.py` and `package_g4b.py` check the retired SteelSeries vendor-wrapper
format. Release builds use `--plain-image`, which links the application at
`0x1000` for the Adafruit bootloader.

## Building and flashing

See [Building from source](BUILDING.md) for the release command and generated
files. See [Updating and recovery](FLASHING.md) to install them by UF2 or SWD.
