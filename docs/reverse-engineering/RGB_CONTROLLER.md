# The RGB controller (IS31FL3743B)

The per-key RGB is an **IS31FL3743B** constant-current matrix driver: 18 current
sinks × 11 switch lines, driving 66 keys × 3 channels. It hangs off **SPIM2** and
is written as a direct-register device — the open firmware does not use a Zephyr
`led_strip` driver for it.

## Bus and pins

| Signal | Pin | Notes |
|---|---|---|
| SCK | P1.09 | SPIM2 clock, 4 Mbit/s |
| MOSI (SDI) | P1.08 | data to the controller |
| MISO (SDO) | P0.08 | the Nordic's SPIM2 MISO line — see *Read-back* below |
| CS | P0.11 | bit-banged in software; the controller's hardware CSN is not used |
| Driver rail | P0.19 | raised last, lowered first |
| Array rail | P0.23 | raised first, lowered last |

The controller latches on the rising edge of CS, so CS rests high between frames.
Its registers are volatile: when either rail drops (Bluetooth idle shutdown) the
chip loses state and must be reconfigured on wake. The open firmware owns the CS
and rail toggles inside `pins_g4b.c` so that file stays the only writer of GPIO
registers.

## Register-page protocol

The controller has paged register space, selected by a leading command byte in
each SPI frame:

| Command | Page | Contents |
|---|---|---|
| `0x50` | PWM | per-channel 8-bit duty (the frame buffer) |
| `0x51` | Scaling | per-channel current scaling |
| `0x52` | Function | global configuration registers |

A function-page write is a 3-byte frame `[0x52, reg, data]`.

### Stock function-page configuration

Three helpers in the stock image program the function page at bring-up:

| Helper | Register | Purpose |
|---|---|---|
| `0x2948a` | `0x00` Configuration | sets the normal-operation bit (clears software shutdown) |
| `0x294c0` | `0x01` Global Current | master current reference for all sinks |
| `0x294ec` | `0x02` Pull select | SWx pull-up / CSy pull-down resistor selection (de-ghosting); value packed as `(a<<4 & 0x70) | (b<<7) | (c & 7)` |

The open firmware mirrors this bring-up in `rgb_g4b.c`.

## SPI read-back and open/short detection — confirmed working

The IS31FL3743B supports SPI register reads, and **P0.08 is physically routed to
the controller's SDO pad on this board** — confirmed on hardware. A read command
is the write command with D7 set: `0xD0` (PWM page), `0xD1` (scaling), `0xD2`
(function page). The transaction is `{read-cmd, start-register}` followed by
clocked dummy bytes; the controller drives its data on SDO from the third byte
on, auto-incrementing the register pointer.

The open firmware normally leaves SPIM2 MISO disconnected
(`PSEL.MISO = 0xFFFFFFFF`, "write-only device"); stock never reads the controller
either. The `apex rgbread` diagnostic (opt-in, `CONFIG_APEX_G4B_RGB_READBACK`)
connects P0.08, proves the link, and reads the detection registers.

### Function-page register map (page `0x52`)

| Reg | Name | R/W | Notes |
|---|---|---|---|
| `0x00` | Configuration | R/W | `[D7:D4 SWS][D3=1][D2:D1 OSDE][D0 SSD]`; run value `0x09` |
| `0x01` | Global Current | R/W | master current |
| `0x02` | Pull select | R/W | de-ghost; `0x33` |
| `0x03`–`0x23` | Open/Short result | R | 33 registers, 6 valid bits each (`D5:D0`) |
| `0x24` | Temperature status | R | `TS` roll-off point + `TROF` current cut |
| `0x25` | Spread spectrum | R/W | |
| `0x2F` | Reset | W | |

### Detection procedure (from `apex rgbread`)

1. Connect P0.08 as a pulled-down input, `PSEL.MISO = 8`.
2. Prove the link: write a sentinel (`0x5A`) to Global Current and read it back.
   Needed because a healthy board's open registers read all-zero — the same as a
   floating-low MISO — so only a sentinel distinguishes wired from unrouted.
3. Light every channel (PWM page `0xFF`; off dots are not scanned), set
   `GCC = 0x0F`, `pull = 0x00` (datasheet detect case 1).
4. Trigger open: Config OSDE `00 → 01` (`0x09` then `0x0B`), wait ≥ 2 scan
   cycles, read `0x03`–`0x23`. Repeat for short with OSDE `10` (`0x0D`).
5. Restore the controller (`g4b_rgb_bringup`) and repaint the frame.

### Result mapping

Each of the 33 result registers holds 6 bits (`D5:D0`) for a group of CS lines;
three registers cover one SW line (CS1–6, CS7–12, CS13–18), 11 SW lines = 33
registers = 198 channels. A set bit is a fault. Channel → coordinate:

```
SW = reg_index / 3 + 1        CS = (reg_index % 3) * 6 + bit + 1
```

Each LED occupies three consecutive CS lines within one SW, stored **B, G, R**.

### Result on the reference unit

This board reads back cleanly. Its open scan reports **17 open channels, 0
shorts**, all accounted for:

- **Five full triplets** (all of B/G/R open) — SW2/CS10-12, SW3/CS16-18,
  SW5/CS13-15, SW6/CS13-15, SW9/CS16-18 — are the five LED slots the driver
  addresses (66) beyond the keys this board has (61): unpopulated positions.
- **One double-open** — SW3/CS1 (B) and SW3/CS2 (G) open, CS3 (R) intact — is a
  physically faulty LED that lights red only. It is chain LED 12, which
  `g4b_led_hid` maps to HID `0x1F`: the number-row **"2"** key.

Every open is explained, which both validates the detection and confirms the
read path is sound.

## Runtime controls

Read/write access to the controller adds these controls, all applied on the
scanner thread (the single SPIM2 writer) and re-applied by `g4b_rgb_bringup()`
after the controller loses state on an idle rail cycle:

| Command | Register | Effect |
|---|---|---|
| `apex rgb gain [0..255]` | Global Current `0x01` | Master current scale across all channels. Unlike lowering PWM it keeps full colour depth, so it is the right brightness/power knob. Persisted. |
| `apex rgb balance <r> <g> <b>` | Scaling page `0x51` | Per-colour current trim (white balance / tint), written as a repeating per-LED `[B,G,R]` scaling pattern under every effect. Persisted. |
| `apex rgbread` | `0x24`, `0x00` | Reports thermal roll-off status and a config-integrity verdict (config `0x00` should read back `0x09`; a mismatch means the last bring-up did not take). |

Gain and balance persist through the debounced `apex/rgb` settings blob
(`{gcc, scale_r, scale_g, scale_b}`) and are re-applied on boot. The open/short
probe forces PWM and scaling to full during detection, independent of the
balance, so `apex rgbread` reports the same faults whatever the tint is set to.
