# Protocols

This page documents the STM32 scanner link, RGB controller, stock USB updater,
and recovery entry. The information comes from stock-firmware disassembly and
bus captures. Unresolved fields are marked as such.

## The STM32 scanner link

The nRF talks to the STM32 over SPIM3 as master, at 4 Mbit/s, SPI mode 0, MSB
first. There is no chip-select — `PSEL.CSN` is left disconnected. Instead the
STM32 gates each transfer with a ready line, and signals pending key data with a
separate attention line.

Two GPIO lines carry the handshake:

- **P0.05 — transfer-ready** (input to the nRF, via GPIOTE, low→high). The nRF
  waits for this before each phase of a transfer. It's a level that holds high
  once the STM32 is up, and drops while the STM32 is busy, so the code checks
  the level first and falls back to the edge.
- **P0.24 — attention** (input). High means the STM32 has a key event queued.
  When it's high the nRF reads a key report; when it's low the nRF sends a
  periodic heartbeat instead. In stock it doubles as a `PORT`/`SENSE` wake
  source while the nRF sleeps.

Every exchange is 64 bytes and runs as two half-duplex phases, not one
full-duplex transfer: the nRF sends 64 bytes with the receive count set to zero,
waits for ready, then reads 64 bytes with the transmit count set to zero. The
first byte of the transmit buffer is the opcode.

### Opcodes

The opcode is `tx[0]`. These are the ones seen in use:

| Opcode | Meaning |
|--------|---------|
| `0xA1` | Read the key bitmap. Polled whenever attention is high. |
| `0xA0` | Travel/heartbeat. Polled when attention is low. |
| `0xA2` | Read raw Hall samples for selected scanner indices. |
| `0x20` | Change or query the scanner state. |
| `0x30`, `0x33` | Write 70-entry per-key actuation tables. |
| `0x35` | Write per-key rapid-trigger sensitivity in tenths of a millimetre. |
| `0x31`, `0x32`, `0x34`, `0x36`–`0x38`, `0x90`, `0xA4` | Other setup commands seen in the stock boot replay. Do not send `0x32`; it writes calibration to flash. |

The STM32's own dispatch is `id = rx[0] & 0x3F`, `dir = rx[0] >> 7` — the top bit
is a read/write direction and the low six bits select the handler. That's why
searching the STM32 image for a literal `0xA2` compare finds nothing; the
handler is reached through a table, not an immediate comparison.

### The key report (`0xA1`)

There is no opcode echo — the reply starts straight into the payload. The
response is a **9-byte key bitmap** in `rx[0..8]`, followed by a status byte
in `rx[9]`, and a second bitmap in `rx[10..18]`. Each bit is one physical key
position; the firmware maps those
positions to a 5×14 ZMK matrix through the `apex_scan_to_matrix` table in
`apex-zmk-g4b/src/kscan_g4b.c`. A few positions are unused, and the bottom row
isn't in scan order — that mapping was recovered empirically from captures.

The interpretation is confirmed against real presses: bit 65 is Fn, bit 17 is E,
bit 16 is W, and holding Fn+E produced exactly `bits [17, 65]`. The stock
firmware additionally parses a second bitmap at `rx[10..18]`; our ingest uses the
first.

The status byte (`rx[9]`) matches the matrix-wide travel scalar returned in
`0xA0` byte 2. Stock routine `fetch_a1` at `0x25080` does not read this offset,
and the value tracks `0xA0` across captures; for example, an `0xA1` value of
`0xCC` was followed by `a0 01 cc`. It is not a validity flag, sequence number,
or count of queued events.

### Stock polling

The stock poll task (`0x256C8`) reads `0xA1` only while attention is high,
exactly once per scheduler tick — roughly a millisecond — and then re-arms
itself. There is no drain loop: a backlog clears across ticks because attention
stays high until the queue empties. Captures show one `0xA1` frame for each press
or release.

The stock task does not debounce, deduplicate, confirm, or rate-limit `0xA1`.
Only the `0xA0` heartbeat is throttled, to one in fifty-one ticks. Taking the
`0xA1` path also defers the next heartbeat.

The bitmap contains absolute levels rather than deltas, so a repeated read is
harmless and a missed read adds latency without losing the final key state.
Debounce happens in the STM32 alongside the Hall threshold. Waiting for a
second matching report would instead stall because attention drops as soon as
the event is consumed.

### Travel scalar (`0xA0`)

`0xA0` returns a status in `rx[1]` (`0x01` when the link is configured) and a
matrix-wide travel scalar in `rx[2]`: bit 7 is meant to indicate a key is down,
and the low bits a travel magnitude. Idle captures often show `0x40`; active
captures show other values. It is not per-key travel.

The STM32 computes per-key analog travel but does not stream it through
`0xA0` or `0xA1`. Per-key values are read explicitly with `0xA2`.

### Per-key analog depth (`0xA2`)

`0xA2` reads raw 12-bit Hall samples from the scanner's 70-key array. Its
handler is `0x08008D8C` in the STM32 3.24.1 image.

```text
tx[0] = 0xA2
tx[1] = count    number of keys, 1..32
tx[2] = start    first scanner index
```

The reply begins at `rx[0]` with `count` little-endian 16-bit values. It has
no opcode echo or header. Larger values mean deeper travel. Bytes after the
requested samples are uninitialized and must be ignored.

The caller must enforce `count <= 32` and `start + count <= 70`; the scanner
still returns a frame when either bound is invalid. A complete read takes three
requests: `(32,0)`, `(32,32)`, and `(6,64)`.

The index matches the `0xA1` bitmap index. W, A, S, and D were verified at
indices 16, 29, 30, and 31. The gamepad reads one window from 16 through 31 and
learns a separate rest and bottom-out range for each of those four keys.

The handler can lower ATTN before replying, so the firmware sends `0xA2` only
when no `0xA1` event is waiting. It does not take a critical section; a response
may contain samples from either side of a scanner pass, although each 16-bit
sample is written atomically.

### The second bitmap (`rx[10..18]`)

The second bitmap uses the same 70-position indexing, but its meaning remains
unknown. The `0xA1` builder reads bitmap0 from scan-block offset `+0x41E`, the
travel scalar from `+0x74F`, and bitmap1 from the adjacent nine bytes at
`+0x427`. Both bitmaps are updated across scans and committed only when their
contents change.

The guards controlling bitmap1 have not been fully decoded, and every capture
contains zeros there. Its one-bit-per-key format rules out analog depth.

### Actuation

Each key has a calibration pair in the STM32's own internal
flash: `LO` is its resting field level and `HI` is bottom-out. They're per unit,
not per model, loaded at boot by the routine at `0x08007FC4` and CRC-protected.

The `0x30`/`0x33` bytes are **not** absolute thresholds. Each is an index into
the same 256-entry table at `0x0800D3E0` that converts travel to tenths of a
millimetre, and the scanner resolves it against that key's own `LO`/`HI` before
comparing. So one number means the same depth on every key and every board,
which is what makes setting it globally sensible.

The table is monotonic but not linear, and it saturates below index 4 (0.2 mm)
and above 0xD3 (3.8 mm, full travel). Some landmarks:

| Index | Depth |
|-------|-------|
| 16 | 1.0 mm |
| 29 | 1.5 mm |
| 46 | 2.0 mm |
| 50 | 2.1 mm — stock's press point |
| 70 | 2.5 mm |
| 110 | 3.0 mm |

The stock actuation table stores `0x32, 0x2E` for each key: 2.1 mm press and
2.0 mm release, or about a tenth of a millimetre of hysteresis. The scanner
performs the comparison, so changing the actuation point requires only a new
table frame, not an analog read. `CONFIG_APEX_G4B_ACTUATION_PRESS` controls that
value.

The press/release assignment is inferred rather than directly named in the
image. The scan loop uses the low byte for its primary comparison, and the stock
table puts the deeper value there. Reversed behavior would indicate that this
assignment needs to be revisited.

If a key's calibration is missing or fails its CRC, the scanner falls back to
the default pair `{928, 3136}`, which puts actuation at a raw level of 1361 —
and any key whose resting level happens to sit above that then reads pressed
forever, because the fallback's auto-ranging only ever lowers `LO`.

### `0x32` writes flash — do not send it

The recalibration path is `0x31 01` to wipe and enter learn mode, a full press
of every key, `0x31 00` to leave, then `0x32` to persist. **`0x32` reaches
`HAL_FLASH_Program` through a function pointer** (`cal_save` at `0x08008114`)
and overwrites the factory per-key calibration in the STM32's internal flash.
There is no undo short of redoing the whole learn pass by hand.

Nothing in the boot replay should send `0x32`. The RAM-only part of the sequence
is recoverable with a power cycle, so calibration experiments can be rehearsed
without committing them to flash.

### Scanner power states

`0x20` controls scanner state; it is not a configuration commit. Configuration
writes take effect immediately. The normal RUN form used at boot replies
`20 02 02` when the scanner is already running. Sending `20 00 01 02` to an
active scanner selects mode 0, hard-stops scanning, and normally replies
`20 00 00`; sending it again while stopped replies `20 02 01`.

`20 03 <period>` changes the alternate scan period without stopping the
scanner. The period is a one-byte millisecond value, so 255 ms is the maximum.
It remains able to detect a key and raise attention, and the setting is held in
RAM only. In the release build this is used only on battery in Bluetooth mode:
50 ms after five seconds without a key event, then 255 ms after one minute. A
key change returns the scanner to its normal cadence; the Nordic also sends
`20 03 01` once attention is clear.

The STM32 image also contains an RTC/mailbox STOP1 loop in task `0x08009672`,
through `0x0800B380`, but the stock nRF does not send a dedicated STOP1 command.
Its link-sleep path (`0x25FB0 -> 0x28ED0`) sends no `0x20` and leaves the scanner
running so P0.24/ATTN can wake the Nordic. Stock routine `0x28EE4` resynchronizes
an already-running link; it is not a STOP1 wake sequence.

Hardware tests found no reliable way to wake mode 0: a key did not wake it, and
driving MOSI did not make READY go low. The open firmware therefore saves power
by lengthening the scanner period while idle, not by entering mode 0 or STOP1.

## RGB (IS31FL3743B)

The controller sits on SPIM2 with chip-select bit-banged on P0.11. Every write
is chip-select low, then `{command, register, data…}`, then chip-select high:

```
command 0x50 → PWM page       (198 brightness registers, addressed 1..198)
command 0x51 → scaling page   (198 current-scale registers, 1..198)
command 0x52 → function page  (configuration registers, addressed literally)
```

On the PWM and scaling pages the register byte is 1-based; on the function page
it's the literal register number. The controller auto-increments its register
pointer within a page, so a whole page goes out as a single framed transfer —
`{0x50, 0x01, <198 bytes>}` for a full frame — rather than 66 per-LED writes.

Bring-up, taken from the stock init:

| Page | Register | Value | Meaning |
|------|----------|-------|---------|
| function (`0x52`) | `0x01` | `0xFF` | global current, full |
| function (`0x52`) | `0x02` | `0x33` | pull-up/down configuration |
| scaling (`0x51`) | `0x01`.. | `0xFF` ×198 | per-channel current scale, full |
| function (`0x52`) | `0x00` | `0x09` | configuration: out of software shutdown |

Set current and scaling first, come out of shutdown last, then write PWM values.
A hardware test confirmed straight R/G/B order in consecutive registers.

## Vendor USB firmware update

> This section documents the **stock** vendor update protocol. The experimental
> [USB installer](../installer/README.md) uses it once to replace the factory
> loader. Open-firmware updates use `APEXBOOT` or SWD; see
> [Updating and recovery](FLASHING.md).

The keyboard exposes SteelSeries' HID update interface on two USB IDs:

| VID:PID | State |
|---------|-------|
| `1038:1626` | Normal application |
| `1038:1627` | Recovery loader |
| `1D50:615E` | Open ZMK application (not part of the vendor protocol) |

An image is `0x4B000` bytes, based at internal flash `0x1C000`, and it carries
its own CRC-32 such that the whole image CRCs to the residue `0x2144DF1C`. The
stock-protocol flasher ([`installer/flash_stock.py`](../installer/flash_stock.py))
sends it in `0x32`-byte HID feature reports to file 11 on filesystem 3. Those
bytes are staged unchanged in the external SPI flash from `0x014000`; each
write is CRC-checked. On reset, the factory loader validates that staged image
and applies it to the internal application slot at `0x1C000`–`0x67000`.

From the normal application, a feature report (`02 00 10` on the updater
interface) asks the app to jump to the loader — which is how the stock software
enters recovery on demand. The open application does not implement this vendor
report; it enters `APEXBOOT` with the key combination or its dedicated serial
interface.

## Recovery entry

The vendor loader lives at flash `0x6E000` and runs before the stock application;
the migration layout is documented in the
[USB installer](../installer/README.md#flash-layout-during-migration). It stays
in recovery after five consecutive watchdog resets, enumerating as `1038:1627`.
It does not read `GPREGRET` or expose another software command for entering
recovery. Current builds use [`APEXBOOT` and SWD](FLASHING.md).

## ZMK Studio

Studio talks to the firmware over its RPC, carried on the USB CDC-ACM serial
port (WebSerial in the browser). The BLE transport is compiled in too, but
there's no host-side bridge for it in this ZMK tree, so serial is the working
path. If Studio cannot reconnect, close any browser tab or serial terminal that
still has the port open, then unplug and reconnect the keyboard.
