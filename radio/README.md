# Shared radio code

The keyboard and receiver use the same packet, handshake and input-queue code.
Build and pairing instructions are in [the receiver README](../dongle/README.md).
The input test builds now use four-channel hopping. This remains development
firmware; the 1 kHz input scheduler and wireless power measurements are unfinished.

## Hop schedule under development

`CONFIG_APEX_RADIO_HOPPING` enables the shared schedule and startup exchange on
both devices. The current map is 2406, 2426, 2450 and 2474 MHz. Both input test
configurations enable it; fixed-channel builds cannot communicate with them.

A map contains 4–16 frequency offsets above 2400 MHz, in ascending order,
at least 2 MHz apart. Offsets must be between 2 and 80. Each 20 ms slot selects
a channel from that map. The session ID chooses a starting offset and a stride
that visits every channel before repeating. This order is not a security
mechanism; packets still require CCM authentication.

The schedule follows elapsed time, not successful packets or acknowledgements.
The keyboard supplies clock updates. A receiver can continue predicting slots
for less than 100 ms without an update, then returns `APEX_HOP_EXPIRED` so the
transport can reconnect. Updates cannot rewind advertised slot time, reuse an
older packet counter, or correct the clock by more than 2 ms. These are initial
test limits. Hardware captures have confirmed successful authenticated traffic
on all four channels. The scheduled transmit END timestamps matched the Nordic
timer captures at 1 microsecond resolution; this does not measure input latency.

### Messages

Both messages travel inside the existing authenticated packet format. Integers
are little-endian. Parsing is tied to the connection's role and session ID.

| Packet type | Payload |
| --- | --- |
| `CHANNEL_MAP` | format `1`, generation (32-bit), count (8-bit), channel offsets |
| `CONTROL` | subtype `0x48`, format `1`, generation (32-bit), slot (32-bit), microseconds into slot (16-bit) |

The receiver accepts only increasing map generations, or an identical map
resent with a fresh encrypted packet counter. Maps are locked once the clock
starts. Replacing a running map currently requires a fresh session; changes
during an active connection are not implemented.

### Startup and reconnection

Disconnected devices scan the four channels. The keyboard dwells for 300 ms
per channel and the receiver for 40 ms. They stop scanning during the handshake.
After authentication, the keyboard offers a map and waits for its acknowledgement.
It then sends clock updates on that channel for five slots before hopping.
It must receive a clock acknowledgement before leaving the starting channel.

Map acknowledgements use CONTROL subtype `0x4d`, format `1`, and a 32-bit map
generation. Clock acknowledgements use subtype `0x49`, format `1`, generation,
and the acknowledged sync packet's 32-bit counter. Both are authenticated.

TIMER2 and PPI channels 17/18 capture radio END events and schedule clock packets.
The keyboard sends a clock update in every slot. Initial guard periods leave
room for transmission and the reply around channel changes.
Clock transmissions may start between 3 and 13 ms into a slot. Once startup
finishes, ordinary input, keepalives and USB-completion acknowledgements use
3–16 ms instead: these packets do not need the clock packet's scheduled lead.
The remaining 4 ms before a hop is a provisional guard, not a measured latency
guarantee. Late USB completions wait for the next permitted window.

Unacknowledged inputs normally use a 5 ms retry timer. A successful acknowledgement
lets the next queued report use the next permitted transmit opportunity; it
does not wait out that retry interval. New input also wakes the radio thread
when the queue was empty. This is not a 1 kHz schedule.
After startup, a fresh clock acknowledgement also allows waiting input to
proceed in the next input window. It does not advance an input sequence or
change the idle keepalive interval; if the queue head was already sent, it
allows an earlier retry. `QUEUE clock_ack_advances` counts these
scheduling decisions. Replayed packets and repeated acknowledgements of the
same or older clock packet cannot trigger them.

The keyboard's `CONFIG_APEX_RADIO_KEYBOARD_EVENT_RX` routes RADIO END through
the PPI channel 17 fork to EGU3/SWI3, waking the radio thread without waiting
for its polling timeout. Bluetooth's direct RADIO interrupt remains untouched;
the event route is armed only after Bluetooth shutdown. EGU3 must not be used
by another driver. `RX_WAKE irq_to_thread_max_us` measures ISR-to-thread delay,
not packet airtime, USB transfer time or physical-key latency.

Receiver USB completion wakes the radio owner thread. A successful report in
the current session can advance the input acknowledgement without waiting for
the keyboard to retry it. Failed transfers and old-session completions cannot
advance it. Lost acknowledgements still recover through ordinary input retries.
Completion replies obey the input window; the guard can delay them until the
next slot. `ACK completion_to_tx_max_us` measures from
USB completion to the end of the local ACK transmission, not reception by the
keyboard. USB callbacks do not run encryption or access the radio peripheral.

A lost connection starts a fresh authenticated session. Input queues discard
old transitions and send the current state. A forced receiver-session reset
and a 500 ms USB submission stall recovered on hardware. One receiver clock
timeout shortly after startup also recovered, but its cause is unresolved.
The user also confirmed typing with keyboard USB disconnected. Longer runs,
interference, idle power and clock drift still need testing.

### Reading the diagnostics

`apex radio_test` on the keyboard and `dongle radio_test` on the receiver show
the same link counters. `HOP_RX` counts accepted packets per channel;
`HOP_SYNC` records received clock updates, their largest gap and the last
control-message error. Ages and gaps are in microseconds; event timestamps
are milliseconds since that device booted. The two devices' uptimes differ.

`RESET reason` is a bit mask: 1 means a full input queue, 2 an output-selection
change, 4 a diagnostic reset request, and 8 an unsuccessful hopping start.
`timeout_at_ms` records the last unrequested timeout. A deliberate reset on
one peer can still cause a timeout on the other, which must notice the silence
before reconnecting. Compare both devices' records before calling that an RF
failure. Rejected traffic can include packets from the session being replaced.

## Tests

`QUEUE_ACK` in keyboard radio diagnostics measures enqueue-to-acknowledgement
time for reports that were acknowledged, including queueing, retries, dongle
USB delivery and the return ACK. It excludes scanner and keymap processing.
Session-start state reports are included; reports discarded on disconnect or
overflow are not. Check the overflow and reset counters alongside it.

`USB_COMPLETE` in `dongle hid_status` measures successful USB submissions in the
current session generation, excluding release reports. Both timing summaries
accumulate until reboot and print count, minimum, maximum and total microseconds.
Their six non-cumulative buckets are <=1 ms, >1–2 ms, >2–5 ms, >5–10 ms,
>10–20 ms and >20 ms. Subtract counts, totals and buckets between snapshots to
compare a test interval; minimum and maximum remain lifetime values. Counts
freeze at UINT32_MAX rather than wrap. These counters contain no key values
and do not measure physical-key-to-application latency or prove 1 kHz cadence.

Run from the repository root with the Python environment used for builds:

```sh
python radio/tests/test_packet.py --workspace ../work/zmk-upstream --output ../work/radio-tests
```

The tests need `unicorn`, `pyelftools` and `cryptography` in that environment.
They cross-check CCM against Python, exercise lost reports and replay rejection,
and test the hop clock with independent clock origins and missing updates.
Startup tests drop map offers, acknowledgements and clock updates, block one
channel, and check that a missing start acknowledgement expires the session.
They do not access the keyboard or measure over-the-air performance.
