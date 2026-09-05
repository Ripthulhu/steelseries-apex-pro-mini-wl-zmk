# Bootloader

An installed keyboard no longer runs the stock SteelSeries loader. It runs the
[Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader),
normally installed over SWD. It supports:

- **UF2 drag-and-drop.** In DFU the board mounts as a mass-storage drive
  (`APEXBOOT`); copy a `.uf2` onto it and it flashes and reboots.
- **Serial DFU.** The classic Adafruit/`adafruit-nrfutil` serial protocol works
  too, over a dedicated CDC port.
- **Bootloader updates.** A board-specific UF2 can update `APEXBOOT` without an
  SWD programmer. It is staged below the running bootloader and handed to the
  Nordic MBR only after the complete file passes the bootloader's checks.

The normal SWD installation boots with APPROTECT open (`0xFFFFFFFF`). The stock
loader refused to launch an application while debug access was open.

The [experimental USB installer](../installer/README.md) is a separate, untested
route for replacing the factory loader without first erasing it over SWD.

## Layout

The current internal and external flash maps are maintained in
[FLASH_MEMORY_MAP.md](FLASH_MEMORY_MAP.md). In the normal SWD-installed layout,
the application starts at `0x1000` and the bootloader starts at `0x74000`.

The [experimental USB installer](../installer/README.md#flash-layout-during-migration)
uses a factory-compatible transition layout instead. Its bootloader starts at
`0x6E000`, leaving a 408 KiB application ceiling until the keyboard is later
rebuilt through SWD.

In the normal SWD layout, UICR contains `NRFFW[0]=0x74000` (bootloader start),
`NRFFW[1]=0x7E000` (MBR parameters), `PSELRESET=0x12` (P0.18),
`NFCPINS=0xFFFFFFFE`, and `APPROTECT=0xFFFFFFFF` (open). The experimental USB
migration deliberately leaves the factory UICR and APPROTECT state unchanged;
the MBR footer selects its `0x6E000` bootloader instead.

The bootloader detects that no SoftDevice is present and launches the
application at the MBR boundary (`0x1000`).

## Board port

The Adafruit bootloader is built for this board from a copy under
`bootloader/apex_pro_mini_wl/` (derived from the `bluemicro_nrf52833` board).
The parts that matter for this hardware:

- **USB and RGB power.** P0.25 enables the U10 USB data path. P0.23 and P0.19
  power the RGB array and controller. `board_init2()` raises all three before
  USB and the DFU indicator start; `board_teardown2()` leaves them high for the
  application. See [Hardware](HARDWARE.md#usb-and-charging-path).
- **Visible DFU status** uses the per-key RGB matrix. The inherited single-LED
  GPIO is assigned to otherwise-unused P1.04, and the neopixel path is disabled
  because P0.07 is the scanner SPI MISO pin.
- **UF2 family ID** `0x621E937A`, volume label `APEXBOOT`, USB `1D50:616F`.
- **Reset input** is P0.18, exposed as `RESET` on the nine-pad CN3 header. Two
  quick low pulses enter DFU.
- The app jump was patched to mask interrupts (`cpsid i`) immediately before the
  `bx` into the application, so a pending interrupt cannot fire during the app's
  early `SystemInit`.

Building the bootloader is documented in [bootloader/README.md](../bootloader/README.md).

## Entering DFU

These entry methods have been tested on the first-generation board. See
[Updating and recovery](FLASHING.md) for the host-side steps.

| Method | How |
|--------|-----|
| **Keymap combo** | Hold `Fn`, then press `Right Ctrl` and `Esc` — a custom `apex_dfu` ZMK behavior |
| **USB magic string** | Host writes `APEXDFU!` to the dedicated DFU CDC port |
| **USB 1200-baud touch** | Open the DFU CDC and switch it to 1200 baud (works with `adafruit-nrfutil` / Arduino-style tools) |
| **Double-tap reset** | Briefly short CN3 `RESET` to `GND` twice |
| **SWD** | `mww 0x4000051C 0x57` then reset (see below) |

While the board is in DFU, the per-key RGB matrix is the only usable indicator
(there is no discrete LED), so it shows DFU state directly:

- **Idle (drive mounted):** a **breathing red background** across every key with
  the **`D`, `F`, `U` keycaps held solid green** on top — their printed legends
  read "DFU". The breathe is animated from `led_tick()` (the SysTick handler, the
  lowest IRQ priority, so its short SPIM write cannot stall USB); `board_rgb_dfu_tick()`
  redraws at ~30 fps over a 2.6 s fade period. (Block-letter glyphs drawn across
  the keys were tried first and are illegible — the staggered layout and the caps'
  own legends break the letterforms.)
- **Writing:** a **red bar fills left→right** across the whole board, tracking the
  UF2 write progress (each written block advances the fill by the LED's horizontal
  position; see `apex_led_x[]` in `pinconfig.c`, mirrored from `rgb_map_g4b.c`).
- **Done:** the **whole board turns green** on the final block (and on
  `STATE_WRITING_FINISHED`) to signal success before the app boots.
- **Drive ejected / normal boot:** dark.

Global LED current is set to max (`0xFF`) for full brightness. The state hook is in
`boards.c` (`led_state` arms the breathe, `led_tick` animates it — patch 0002) plus
the write-path call in `ghostfat.c` (patch 0001); the drawing primitives
(`board_rgb_dfu_glyph`, `board_rgb_dfu_tick`, `board_rgb_progress`) live in
`bootloader/apex_pro_mini_wl/pinconfig.c`. A single-writer lock guards the shared
framebuffer/SPIM2 between the `led_tick` ISR and the one-shot writers. The UF2
application floor and self-update handling are documented in
[bootloader/README.md](../bootloader/README.md#local-changes).

`INFO_UF2.TXT` on the drive includes the last reset cause, the A/B recovery
state, and a one-line summary of the newest valid crash record. If a crash has
been recorded, `CRASH.BIN` contains its 248-byte raw record. Copy that file off
the drive if you need it for debugging.

### DFU reset register

In this ZMK/Zephyr tree, `sys_reboot(type)` **does not** set the retention
register the bootloader reads. `CONFIG_NRF_STORE_REBOOT_TYPE_GPREGRET` is an
unused Kconfig symbol here, and `sys_arch_reboot()` is the `__weak` stub that
ignores its argument. So ZMK's stock `&bootloader` behavior would *not* reach
DFU on this board.

Every DFU path therefore writes the magic **directly**:

```c
NRF_POWER->GPREGRET = 0x57u;   /* 0x57 = DFU_MAGIC_UF2_RESET */
__DSB();
NVIC_SystemReset();
```

`GPREGRET` lives at `0x4000051C`; `0x57` is what the bootloader's
`check_dfu_mode` reads to stay in DFU. You can prove the mechanism from a
debugger: `mww 0x4000051C 0x57` then reset drops straight into DFU. The key
combination is provided by the `apex_dfu` behavior. The serial magic string and
1200-baud signal require `CONFIG_APEX_G4B_DFU_TRIGGER=y`, the current USB CDC-ACM
stack, and `CONFIG_UART_LINE_CTRL=y`.

## Restoring stock

The open stack can be rebuilt or restored from a matching full-flash/UICR backup
over SWD; see [SWD_RECOVERY.md](SWD_RECOVERY.md). A byte-exact stock restore is
not possible with the files currently available. APPROTECT prevents reading the
proprietary factory loader over SWD, disabling it erases the flash, and the
available SteelSeries updater files do not contain that region. If a complete
stock flash/UICR pair becomes available, restore its UICR last because it
re-enables APPROTECT.
