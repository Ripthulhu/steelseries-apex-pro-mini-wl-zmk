# The 2.4 GHz dongle link

The first-generation Apex Pro Mini Wireless ships with a USB receiver (dongle,
`1038:1624`) that the keyboard talks to over a 2.4 GHz radio link. Both ends are
Nordic nRF52 devices. This page records the parts of that link recovered so far
and what is still missing from the open implementation.

## Key findings

- **Both ends drive the radio directly.** The link is an Enhanced
  ShockBurst-compatible protocol implemented against the nRF `RADIO` peripheral
  rather than BLE, a SoftDevice, or Nordic's ESB/Gazell library.
- **There is no over-the-air encryption.** Neither firmware touches CCM/ECB/AAR.
  Reimplementing the link therefore does not require an encryption key.
- **The private radio address is a fixed compile-time constant** — no FICR
  device ID, `DEVICEADDR`, or UICR customer value is used to derive it. Devices
  are distinguished at the protocol layer by a 2-byte pairing ID and device
  index, not by their radio address. Other link parameters are negotiated at
  pairing time and are still missing.

All of this was recovered by static analysis of the stock keyboard's radio driver
(disassembly of the link-init and pairing routines), cross-checked against USB
captures of the dongle and radio tests on the development keyboard.

## Operational PHY

These are the registers the stock keyboard's established-link initialization
writes. The unfinished probe in `radio_esb_g4b.c` also uses the private address
during pairing, but does not yet implement this complete operational path.

| Register | Value | Meaning |
|----------|-------|---------|
| `MODE` | `4` | `Ble_2Mbit` (2 Mbit GFSK, BLE framing) |
| `BASE0` | `0x76412900` | written directly, no bit-reversal |
| `PREFIX0` (AP0) | `0x71` | logical address 0 only |
| `TXADDRESS` | `0` | |
| `RXADDRESSES` | `1` | pipe 0 |
| `CRCCNF` | `0x103` | 3-byte CRC (CRC-24), skip address |
| `CRCPOLY` | `0x65B` | |
| `CRCINIT` | `0x00FFFFFF` | |
| `PCNF0` | `0` | static length — no on-air LENGTH field |
| `PCNF1` | `0x030300FE \| (STATLEN<<8)` | MAXLEN 254, **BALEN=3 → 4-byte on-air address**, big-endian, **WHITEEN=1** |
| `DATAWHITEIV` | `0x40` | never written by firmware → the nRF reset value (bit 6 = mandatory LFSR 1, seed bits 0) — channel-independent whitening |
| TXPOWER | `+4 dBm` | |

Because `DATAWHITEIV` is left at reset and `WHITEEN=1`, the hardware whitens and
CRCs identically to stock without any custom setup: configure these registers,
drop the plaintext frame in the buffer, and transmit.

With `BALEN=3`, the 4-byte on-air address is the
low 3 bytes of `BASE0` plus `PREFIX0` — the `0x76` high byte is not on air.

### Channels

The channel plan is a **runtime adaptive sweep**, not a static table. The
keyboard's swept set matches what we observe on air: **2, 3, 4, 24, 25, 26**
(`FREQUENCY = channel | 0x100`, the MAP bit). The active channel window
(min/max) is *assigned by the dongle at pairing time* in a config message and
lives only in RAM — the keyboard doesn't know its channels until it has paired.

## Airframe and protocol

- **Header** (before the HID body): `byte[0]` = frame/pipe sequence counter
  (rolls), `byte[1]` = mirror of `byte[2]`, `byte[2]` = 2-bit rolling PID
  (ack/seq), `byte[3]` = parity/flags. Then the report body.
- **Six report modes**: command, consumer, standard boot keyboard
  (`[mod][resv][kc1..6]`), NKRO keyboard (32-byte bitmap, modifiers `0xE0`–`0xE7`
  as bits of byte 28), mouse, sync. `STATLEN` = body + link header, per mode.
- **Pairing** is a symmetric opcode-nibble protocol (dispatch on
  `frame[opcode] & 0xF0`; classes `0x00/10/20/40/50/60/70/80/D0`). The keyboard
  sends class `0x40/0x50/0x60` requests carrying its runtime 2-byte pairing id;
  the dongle replies class `0x80` config: channel window, negotiated
  4-byte link value, and device index (≤8 pipes). After bonding the keyboard
  auto-reconnects to the stored id/channel — no per-connection user action.

## Pairing

The fixed radio settings above were recovered from both firmware images and
checked on hardware. Pairing itself was traced in the keyboard and dongle code,
but several runtime values and timing details are absent from the images. A live
radio capture is still needed.

**Pairing is a two-step handshake across two different radio addresses**, not the
operational ESB link:

1. **Beacon.** The keyboard broadcasts on the **BLE advertising access address
   `0x8E89BED6`** (register form `BASE0=0x89BED600`, `PREFIX0=0x8E`) on the BLE
   advertising channels **37/38/39** (nRF `FREQUENCY` 2/26/80). This is BLE PHY
   but *not* a spec-compliant advertisement — MODE `Ble_2Mbit`, static length,
   `WHITEEN=0`, CRC-24 poly `0x65B`, and the CRC-init bound to this address is
   **`0x00FFFFFF`** (the dongle image contains a `0x00555555` CRC overlay too, but
   it is not the one wired to this link).
2. **Private link.** The dongle records the beacon and the link **moves to the
   private address `0x76412900 / 0x71`**, where the class-`0x40`/`0x80` exchange
   above completes the bond.

The dongle's receive path performs no beacon content check. There is no
SteelSeries company-ID match and no device-type
`{02 01 01}` comparison in its receive path — a beacon is accepted purely on
PHY, CRC, and framing. The payload bytes do not matter; only the radio
config and length must line up.

The radio, address encoding, and receive path were checked on hardware. In
`Ble_1Mbit` on `0x8E89BED6`, the self-test in `radio_esb_g4b.c` receives roughly
60–70 ambient BLE advertisements per second. Tests sent the beacon, the
class-`0x40` request, and the complete two-phase sequence on the recovered PHY;
the SteelSeries software reported pairing failure each time.

The missing pieces are runtime state that static analysis cannot recover:

- the exact **STATLEN** of each frame (a runtime block-load, not a constant),
- the private link's **channel** (a dongle-assigned runtime hop table), and
- the **microsecond-precise TX→RX turnaround** — the stock firmware sequences it
  with PPI + a hardware timer (TIMER2 `CC` `0x310`/`0x360`); a software loop
  listens at the wrong instant even when the frames are right.

A sniffer capture is still needed. An SDR or spare nRF52 can record the stock
keyboard's advertising-channel pairing traffic and private address. The capture
must come from a keyboard still running the factory firmware; the development
board cannot currently be restored to that state.

## On-device implementation

- `apex-zmk-g4b/src/radio_g4b.c` stands the BLE controller down (`bt_disable` →
  controller deinit) when the mode switch is in the dongle position, freeing
  `NRF_RADIO` for our driver. Moving the switch across the dongle boundary
  triggers a reset so only one driver initializes `NRF_RADIO`.
- `apex-zmk-g4b/src/radio_esb_g4b.c` contains TX/RX primitives and the unfinished
  two-phase pairing probe. It is not a working keyboard-to-dongle transport.

Both are gated behind `CONFIG_APEX_G4B_DONGLE_RADIO` and
`CONFIG_APEX_G4B_ESB`. Release firmware leaves them disabled.
