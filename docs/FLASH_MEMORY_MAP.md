# Flash layout

The keyboard has 512 KiB of internal nRF52833 flash and a 1 MiB FM25Q08A SPI
flash. This page describes the normal layout installed over SWD. The untested
[USB migration](../installer/README.md#flash-layout-during-migration) uses a
bootloader at `0x6E000` and has a smaller application slot. Addresses
below are byte offsets from the start of each device.

## nRF52833 internal flash

| Region | Range | Size | Contents |
|---|---|---:|---|
| MBR | `0x00000..0x01000` | 4 KiB | Nordic Master Boot Record |
| Application | `0x01000..0x72000` | 452 KiB | ZMK firmware |
| Free | `0x72000..0x74000` | 8 KiB | Former internal NVS partition |
| Bootloader | `0x74000..0x7E000` | 40 KiB | Adafruit nRF52 bootloader |
| MBR parameters | `0x7E000..0x7F000` | 4 KiB | MBR command page |
| Bootloader settings | `0x7F000..0x80000` | 4 KiB | DFU state and application metadata |

The application starts immediately after the MBR at `0x1000`. The stock keyboard
used S113, and early versions of this port used an S140-based layout with the
application at `0x27000`. The installed firmware has no SoftDevice; Bluetooth
uses Zephyr's controller. Moving the application down to `0x1000` also recovered
the 12 KiB of RAM that the S140 layout reserved.

The 8 KiB gap below the bootloader is unused. Extending the application into it
would also require expanding the external A/B slot and updating the layout
checks in both the application and bootloader.

UICR boot addresses, reset-pin assignment, and debug settings are documented in
[BOOTLOADER.md](BOOTLOADER.md).

## External SPI flash

| Range | Size | Contents |
|---|---:|---|
| `0x00000..0x60000` | 384 KiB | Stock SteelSeries data and staging area; unused by current firmware |
| `0x60000..0x68000` | 32 KiB | ZMK settings and Bluetooth bonds |
| `0x68000..0x69000` | 4 KiB | NVS provision marker |
| `0x69000..0x6A000` | 4 KiB | A/B descriptor |
| `0x6A000..0x6B000` | 4 KiB | Boot-failure tally |
| `0x6B000..0x80000` | 84 KiB | Reserved; optional LittleFS development area |
| `0x80000..0x82000` | 8 KiB | Flash diagnostics |
| `0x82000..0x8B000` | 36 KiB | Unassigned |
| `0x8B000..0xFC000` | 452 KiB | A/B recovery image |
| `0xFC000..0x100000` | 16 KiB | Coredump ring |

The external device stores data only; the nRF cannot execute firmware from it.
The recovery image has the same 452 KiB limit as the internal application
partition.

The release verifier checks the internal partition, external A/B slot, linker
symbols, and bootloader constants together.
