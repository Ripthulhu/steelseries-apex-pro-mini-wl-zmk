# Hardware

This page describes the first-generation Apex Pro Mini Wireless board used for
the port. Later revisions may differ. It uses a Nordic controller for the
keyboard and radio, plus a separate STM32 controller for the Hall-effect
switches. This project replaces only the Nordic firmware.

## Board overview

```text
                                USB-C
                                  │
                     ┌────────────┴────────────┐
                     │ U10 USB data-path switch│
                     └────────────┬────────────┘
                                  │ USB
  mode switch ──ADC──┐      ┌─────┴───────────────┐      2.4 GHz antenna
                     ├──────│ nRF52833 module M2  │──────────────┐
  BQ25895 charger ─I²C┤      │ ZMK and bootloader │              │
                     │      └─┬─────┬─────┬──────┘              │
  external NOR ────SPI┘        │     │     │                     │
                               │     │     └─SPI── IS31 RGB driver
                               │     │
                               │     └─SPI + control── STM32G0 scanner
                               │                         │
                               └──── attention IRQ ─────┘
                                                         │
                                                  61 Hall switches
```

## Identified hardware

| Part | Marking or designator | Function | Connection to nRF52833 |
|---|---|---|---|
| Nordic nRF52833 | RF module `M2`, marked `SBA0526V05` | ZMK, USB, Bluetooth, keymap, lighting, storage, and power policy | Main controller |
| STM32G0, 128 KiB flash | `U1`; exact G070/G071 suffix not confirmed | Hall-sensor sampling, calibration, actuation, rapid trigger, and key reports | SPIM3 plus ready and attention lines |
| ISSI IS31FL3743B | RGB controller | Drives 66 RGB LEDs through 198 PWM channels | SPIM2 and two power-control lines |
| FM25Q08A-compatible NOR | 1 MiB external flash | ZMK settings, Bluetooth bonds, recovery image, and crash records | SPIM0 |
| TI BQ25895 | `U31` | Battery charging, power-path control, and battery ADC | TWI1/I²C at address `0x6A` |
| USB data-path switch | `U10`; exact part not confirmed | Connects the USB-C data pair to the Nordic when enabled | Controlled by P0.25 |
| Battery pack | Fuji 4867A0 | 5870 mAh, 3.85 V nominal, 4.4 V charge limit | BQ25895 power path |
| Three-position switch | Bluetooth / USB / 2.4 GHz | Selects operating mode through a resistor divider | SAADC AIN1 on P0.03 |
| Radio clock and antenna | 32 MHz crystal on M2, u.FL at `J2` | nRF radio reference and external antenna connection | Inside or beside M2 |

Photographs of marked and unidentified devices are kept in
[`hardware/photos/`](../hardware/photos/). Exact identities are only stated
where the package marking or electrical behavior supports them.

## Processor responsibilities

### Nordic nRF52833

The Nordic is the only processor replaced by this project. It runs:

- the ZMK application and keymap;
- USB and Bluetooth HID;
- the UF2/serial bootloader;
- the master side of the STM32 scanner link;
- RGB effects and RGB power control;
- external-flash storage and A/B rollback;
- charger configuration and battery reporting; and
- the mode-switch and wireless idle policy.

The stock Nordic is protected by APPROTECT. The normal SWD installation erases
the Nordic and installs an open UICR configuration, leaving SWD available for
later debugging and recovery. The untested USB migration keeps the factory UICR
and protection state. See [INSTALL.md](INSTALL.md).

### STM32G0 scanner

The STM32 retains its SteelSeries firmware and per-key calibration. It reads the
Hall sensors, applies the configured actuation and rapid-trigger behavior, and
returns key bitmaps and travel data to the Nordic.

The STM32 is an SPI peripheral; the Nordic is the master. Configuration and key
reports use fixed 64-byte frames. The scanner raises P0.24 when a key event is
waiting, allowing the Nordic to sleep between events. The complete protocol is
in [PROTOCOL.md](PROTOCOL.md).

An SWD check confirmed RDP Level 1: peripheral registers remain readable, but
reads from the STM32 flash fault. Lowering that protection performs a mass
erase. The SteelSeries update files found during this work contain the scanner
application, but not its factory loader or this keyboard's per-key calibration,
so this project leaves the STM32 untouched.

## Debug headers

### Nordic CN3 — use this header

The Nordic header is a row of nine round pads under the space bar. Remove only
the space-bar keycap to reach it; the keyboard case stays fully assembled.

```text
VDD  SWDCLK  SWDIO  GND  RESET  UTX  URX  DTM  SWO
```

![Nordic CN3 under the space bar](../hardware/photos/batch2-cn3.png)

| Pad | Nordic signal | Use |
|---|---|---|
| `VDD` | 3.3 V target rail | Voltage reference for probes that require VTref; do not use as probe power |
| `SWDCLK` | SWD clock | SWD programming and debugging |
| `SWDIO` | SWD data | SWD programming and debugging |
| `GND` | Ground | Required common reference |
| `RESET` | P0.18 / nRESET | Optional hardware reset and bootloader double-reset |
| `UTX` | P0.10 | 19,200 baud diagnostic UART output |
| `URX` | P0.09 | 19,200 baud diagnostic UART input |
| `DTM` | Unconfirmed test function | Do not ground or drive; grounding stopped the board |
| `SWO` | P1.00 | Trace output configured by the stock firmware |

Follow [INSTALL.md](INSTALL.md) for wiring. Normal SWD needs only `SWDCLK`,
`SWDIO`, and `GND`.

### STM32 five-pad header — do not use for installation

```text
VMCU  SWDIO  SWCLK  NRST  GND
```

![Protected STM32 debug header](../hardware/photos/batch2-mcu-swd.png)

The `VMCU`, `SWCLK`, and `NRST` naming identifies this as the STM32 header. It
is unrelated to Nordic installation or recovery.

## Communication buses

| Interface | Signals | Device | Notes |
|---|---|---|---|
| SPIM3 | P0.04 SCK, P0.06 MOSI, P0.07 MISO | STM32 scanner | SPI mode 0; P0.05 ready and P0.24 attention are separate control inputs |
| SPIM2 | P1.09 SCK, P1.08 MOSI, P0.08 MISO, P0.11 CS | IS31FL3743B | 4 Mbit/s; chip select is controlled in software |
| SPIM0 | P0.27 SCK, P0.00 MOSI, P0.01 MISO, P0.26 CS | External 1 MiB NOR | Shared by settings, A/B recovery, and crash records |
| TWI1/I²C | P0.16 SCL, P0.17 SDA | BQ25895 at `0x6A` | 400 kHz in stock firmware; current driver uses 100 kHz |
| SAADC | P0.03 / AIN1 | Mode-switch divider | Approximately 0 V, 1.6 V, and 3.3 V for the three positions |
| USB | D+ and D− through U10 | USB host | U10 must be enabled through P0.25 before enumeration |
| Radio | Internal nRF52833 peripheral | Bluetooth; experimental 2.4 GHz work | Bluetooth is supported; the SteelSeries dongle protocol is incomplete |

SPIM0 and the low-frequency crystal function share P0.00/P0.01. The board uses
the nRF internal RC source for LFCLK so those pins remain available to the NOR.
The radio uses its separate 32 MHz crystal.

## Confirmed Nordic pin map

| Pin | Function |
|---|---|
| P0.00 | External-NOR MOSI |
| P0.01 | External-NOR MISO |
| P0.02 | STM32 enable |
| P0.03 | Mode-switch ADC input |
| P0.04 | STM32-link SCK |
| P0.05 | STM32 transfer-ready input |
| P0.06 | STM32-link MOSI and link-resynchronization line |
| P0.07 | STM32-link MISO |
| P0.08 | RGB-controller MISO |
| P0.09 | Diagnostic UART RX |
| P0.10 | Diagnostic UART TX |
| P0.11 | RGB-controller chip select |
| P0.16 | Charger I²C SCL |
| P0.17 | Charger I²C SDA |
| P0.18 | nRESET |
| P0.19 | RGB-driver power control |
| P0.20 | Charger-side input; exact function unconfirmed |
| P0.23 | RGB-array power control |
| P0.24 | STM32 attention and wake input |
| P0.25 | USB data-path enable |
| P0.26 | External-NOR chip select |
| P0.27 | External-NOR SCK |
| P0.28 | Charger control, active low |
| P1.00 | SWO trace output |
| P1.05 | Second STM32 enable |
| P1.07 | Stock-firmware boot strap; not connected to a confirmed user pad |
| P1.08 | RGB-controller MOSI |
| P1.09 | RGB-controller SCK |

The application pin definitions are in
[`pins_g4b.h`](../apex-zmk-g4b/src/pins_g4b.h). Pins not listed there are not
assumed to be free.

## RGB power and data path

The IS31FL3743B controls 66 LEDs with three channels per key. The Nordic sends a
198-byte PWM page over SPIM2. P0.23 is enabled before P0.19; shutdown uses the
reverse order. The controller loses its register state when its rail is off, so
the application initializes it again after wake.

Bluetooth idle shutdown disables both RGB rails and parks SPIM2. The STM32
enable lines stay active so P0.24 can wake the Nordic on a key event.

## USB and charging path

The USB-C data pair passes through U10 before reaching the Nordic. P0.25 enables
that path. A correctly configured USB peripheral cannot enumerate while U10 is
off.

The BQ25895 manages charging and the system power path. It also measures battery
voltage and charge current. The application uses one-shot ADC conversion for
battery reporting and limits writes to named charger operations. The battery is
rated for a 4.4 V charge limit; charger constants must not be copied to a board
with a different cell.

See [FEATURES.md](FEATURES.md#battery-charge-limit) for the charge limit and
hold-band behavior, and [PROTOCOL.md](PROTOCOL.md) for scanner and RGB packet
details.

## Memory devices

The nRF52833 has 512 KiB of internal flash and 128 KiB of RAM. The external NOR
is 1 MiB. Current internal and external partitions are documented in
[FLASH_MEMORY_MAP.md](FLASH_MEMORY_MAP.md).

The stock and open Nordic layouts are different. Use the current
[flash map](FLASH_MEMORY_MAP.md) when checking an open-firmware image.

## Known hardware uncertainties

- The exact STM32G0 suffix is not confirmed.
- The exact U10 USB-switch part is not confirmed.
- P0.20 and some unpopulated or support-device functions remain inferred.
- The `DTM` pad function is not confirmed and must not be driven.
- The proprietary 2.4 GHz pairing protocol remains incomplete.
