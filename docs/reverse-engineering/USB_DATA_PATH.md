# The USB data-path switch (U10)

U10 sits between the USB-C data pair and the Nordic's USB pads. It is a single
switch, not a mux: there is no select line, only one control input on **P0.25**.
Driving P0.25 high connects the data pair to the Nordic; driving it low isolates
the Nordic from the port. VBUS and charging are unaffected by P0.25. The switch
gates data only, so the BQ25895 keeps charging with the data path cut.

Everything here was recovered from the stock Nordic image
(`apex_pro_mini_wireless_nordic_3.24.1`); addresses are in that image's space.

## Control register writes

| Action | Address | Instruction | Effect |
|---|---|---|---|
| Configure pin | `0x25646` | `str #3 -> [0x50000000 + 0x764]` | `PIN_CNF[25] = 3` (output, input buffer disconnected) |
| Connect | `0x2564a` | `str 0x02000000 -> [0x50000000 + 0x508]` | `OUTSET` bit 25 -> P0.25 **high** |
| Cut | `0x262f8` | `str 0x02000000 -> [0x50000000 + 0x50c]` | `OUTCLR` bit 25 -> P0.25 **low** |

`0x764` is `PIN_CNF[25]` (`0x700 + 25*4`), `0x508` is `OUTSET`, `0x50c` is
`OUTCLR`. Bit 25 is `0x02000000`. The pin is configured once and thereafter only
its level is toggled.

## The apply function (`0x26204`)

A single function drives the switch. Its first argument decides the direction:

```
26204  push {r3-r9,lr}
2620a  r9 = &conn_mode          ; RAM byte at 0x200094C2
26212  r5 = arg0                ; 0 = cut, non-zero = connect
...
26228  r0 = conn_mode
2622c  r0 = (conn_mode == 0)    ; clz/lsr idiom
2623a  conn_mode == 1 -> sub-path 0x26312
26240  conn_mode == 2 -> sub-path (bl 0x24128)
26248  cmp r5, #0
2624a  r5 == 0 -> 0x262ec       ; CUT: OUTCLR bit25, then return
       r5 != 0 -> settle loop   ; ~10 * 64000-cycle spins, leaves P0.25 high
```

Call sites:

| Caller | Argument | Meaning |
|---|---|---|
| `0x2642e` | `1` | connect |
| `0x26460` | `1` | connect |
| `0x2646c` | `1` | connect |
| `0x26784` | `1` | connect |
| `0x26aa2` | `0` | **cut** |

Four sites connect (all on the USB bring-up / attach paths); exactly one cuts.

## When stock cuts the data path

The single cut site lives inside a transport event handler. `0x26aa0` is the
branch target that starts the cut block; the `bl apply(0)` itself is at `0x26aa2`:

```
269e0  push {r4-r8,lr}
269e4  r6 = &conn_mode          ; same RAM byte 0x200094C2
269e6  r3 = conn_mode
269ea  r4 = arg0                ; event code
269ec  conn_mode == 0 -> skip   ; nothing active, no cut
269ee  cmp arg0, #1
269f0  arg0 == 1 -> 0x26aa0     ; -> apply(0)  == CUT U10
```

So the cut fires when **a transport is active (`conn_mode != 0`) and the handler
receives event `1`**, the transition that takes the active output away from
USB. In practice this is the charge-only case: the cable stays in for power, but
the keyboard has switched its output to Bluetooth or the 2.4 GHz dongle, so stock
drops the USB data path to stop the host from seeing an HID device it can no
longer reach. It is not a VBUS-loss handler; VBUS removal is handled separately.

## The connection-mode byte (`0x200094C2`)

`conn_mode` is the central transport-selection variable. It is read or written
in 21 places across the transport state machine and takes values `{0, 1, 2}`
(idle/none, and the two active transports). Both the apply function and the cut
handler key off it. The exact `1`/`2` assignment (BLE vs. dongle) was not pinned
down from the stripped image and is not needed to reproduce the policy: the cut
depends only on `conn_mode != 0` plus the event, and the connect sites are the
USB attach paths.

## Relationship to the open firmware

The open firmware uses the same pin under the name `G4B_RAIL_USB` (P0.25). Three
call sites touch it:

- `pins_g4b.c` `g4b_usb_rail_pulse()` drives P0.25 low then high to force a USB
  re-enumeration, and always restores it high.
- `pins_g4b.c` `g4b_usb_rail_set()` sets the level directly for the VBUS gate.
- `link_g4b.c` drives the level from VBUS detection on the scanner tick.

The open firmware does **not** reproduce stock's charge-only cut, because its
USB CDC endpoints (shell, DFU, Studio RPC) must stay reachable while a cable is
plugged, even when the output is Bluetooth. Instead it drives P0.25 from the
Nordic's `USBREGSTATUS.VBUSDETECT`: the data pair is connected whenever the port
carries voltage and isolated on battery. A cable therefore always powers the
data path (debug endpoints survive), while the pair is still isolated when there
is no host to reach, the safe half of stock's behaviour.

The pin boots high, and the gate only drops it on confirmed battery operation,
so it never gates the first enumeration or DFU recovery. This is
`CONFIG_APEX_G4B_USB_DATA_VBUS_GATE` (default on); turning it off holds P0.25
high at all times, the prior behaviour. See
[CONFIGURATION.md](../CONFIGURATION.md).
