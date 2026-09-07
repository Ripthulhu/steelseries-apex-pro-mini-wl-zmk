# Shared radio code

The keyboard and receiver use the same packet, handshake and input-queue code.
Build and pairing instructions are in [the receiver README](../dongle/README.md).
The input test builds now use four-channel hopping. This remains development
firmware; the 1 kHz input scheduler and wireless power measurements are unfinished.

## Input delivery changes

Input format 2 separates reception from USB completion. Both devices need this
format; older input builds cannot exchange reports with it. Pairing storage and
the encrypted session format are unchanged.

The receiver buffers up to eight reports in order, including the report being
sent over USB. Its authenticated reply contains the highest accepted sequence,
highest USB-completed sequence, remaining queue space and host LED state. The
keyboard can send the next report after acceptance, but retains its copy until
USB completion. A full receiver refuses another report without advancing its
accepted sequence. Failed USB transfers leave the queue head available to retry.

The 11-byte ACK payload is: format `2`, accepted sequence (32-bit), completed
sequence (32-bit), free entries (8-bit), LEDs (8-bit). Integers are little-endian.
The sender rejects regressing positions, acceptance beyond what it sent and
inconsistent queue counts. Session changes clear both queues and resend current
input state. USB interface or report-protocol changes request a fresh session.

Completion is returned in the next input or keepalive reply. The receiver does
not transmit a separate completion ACK that could collide with the next input.
USB callbacks neither encrypt packets nor access the radio.

When several reports are already queued, the keyboard sends up to three in one
authenticated `INPUT_BATCH` packet (type 6). It does not wait to collect a batch;
a lone report uses the original single-report packet. The batch is limited by
the receiver's advertised free entries. If those credits cannot fit the queued
batch, the sender polls for credits while the receiver drains its existing USB
reports. This avoids reducing a backlogged stream to one-report packets.
One cumulative ACK covers the accepted
prefix, and the keyboard retains every report until USB completion.

The payload starts with format `2` and an 8-bit count, followed by one to three
ordinary input frames with consecutive sequence numbers. Each frame retains
its type and full HID report, so a press and release are never merged. The
largest payload is 56 bytes. The receiver validates the entire batch before
delivery, skips already accepted prefixes on retry, and stops if its FIFO fills.
Malformed batches cannot deliver a valid-looking prefix. Both devices need
batch-capable firmware; older receivers reject packet type 6.

The keyboard prepares a single subsequent input packet while waiting for a reply
when only one subsequent report is queued. Batches are encoded together at the
send site.
It uses that packet only if the report sequence, session ID and reserved TX
counter still match. Encoding intervening traffic invalidates it; discarded
counters are never reused. Retries still receive fresh counters. `PREPARE`
reports how many packets were prepared, used or discarded. This moves some
encryption work ahead of the next reply without changing the wire format.

The receiver prepares an ACK for the next accepted report while listening.
After authenticating and accepting that report, it compares the actual queue
positions, credits and LEDs with the prepared payload. A changed completion,
LED state, session or TX counter forces fresh encryption. It never predicts
USB completion or sends a prepared reply before authentication. `PREPARE_ACK`
counts prepared, used and discarded replies; each can be transmitted only once.

USB reception and completion events wake the submission thread immediately.
A 1 ms timeout remains for retrying temporary submission failures. TX completion
interrupts rearm radio reception before waking the radio thread. Separate DMA
buffers keep the transmitted packet apart from a newly arriving response.

Packet authentication and reply preparation still run in the radio thread.
The sender waits for authenticated reception before sending the next packet,
but each packet can now contain several reports. A sliding window of independent
radio packets and a fixed 1 kHz transmission schedule are not implemented.
Nordic's [ESB implementation](https://github.com/nrfconnect/sdk-nrf/tree/main/subsys/esb)
is the reference for turnaround and buffering. Integrating it requires checking
our pinned dependencies and its TIMER/PPI/interrupt ownership; it is not enabled
in these builds.

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
older packet counter, or correct the clock by more than 500 us. These are initial
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
Both devices request the high-frequency crystal through Zephyr's clock manager
and wait for startup before enabling RADIO. Hardware captures show HFCLKSTAT
`0x00010001` (running, crystal selected). TIMER2 divides the high-frequency
peripheral clock to 1 MHz; retry and hop deadlines do not use the low-frequency
RC clock. Both builds use calibrated LFRC for kernel/sleep timing. Nordic requires
HFXO for RADIO operation; see [CLOCK](https://docs.nordicsemi.com/r/bundle/ps_nrf52833/page/clock.html).
On the keyboard, PPI channel 19 routes DISABLED to EGU3 for completion interrupts.
The keyboard sends a clock update in every slot.
TIMER2 compare 0 wakes the radio thread for the earliest retry, input-window
opening, clock-window opening or hop boundary. The interrupt only wakes the
thread, so packet processing and thread scheduling can still delay a switch.
`HOP_TIMER` counts timer interrupts and deadlines missed while arming the timer.
The count now includes retry/window deadlines, not just channel changes. Receiver
idle waits can exceed 10 ms, so `loops_over_10ms` alone no longer indicates a stall.
Missed deadlines wake the thread immediately instead of waiting for timer wrap.
Clock transmissions may start between 3 and 13 ms into a slot. Once startup
finishes, ordinary input and keepalives use 0.75–18.5 ms instead: these packets
do not need the clock packet's scheduled lead. Replies may start until 19.5 ms,
so the receiver can acknowledge input accepted near the input cutoff.
The 0.75 ms opening guard and 1.5 ms closing guard leave a 2.25 ms gap across
each channel change for new input. Replies retain a 0.5 ms closing guard for
ramp-up and airtime. These margins are experimental, not worst-case timing bounds.
Late USB completions are reported in the next solicited reply.
Immediately before starting an input, keepalive or input-ACK transmission,
the radio owner rechecks the window and selected channel with interrupts
briefly locked. If it is too late to send, the radio returns to receive mode
and leaves delivery to the normal retry handling. `TX window_deferrals`
counts these cases. Each retry uses a fresh encryption counter.
`HOP_TIMING switch_phase_max_us` records how far into a slot the receive-enable
request was made after a channel change. It excludes receiver ramp-up.
`correction_max_us` records the largest accepted clock adjustment. Both are
lifetime maxima. They help check the timing margins but cannot prove that
every channel change finished on time.

Pending input uses a 1.5 ms retry deadline measured from TX completion; idle
keepalives use 5 ms. A successful acknowledgement
lets the next queued report go out without waiting for that timer.
New input also wakes the radio thread when no radio reply is outstanding and
all previous reports have been accepted, even if USB completion is still pending.
This is not a 1 kHz schedule.
`CONFIG_APEX_RADIO_THREAD_PRIORITY` sets the preemptible radio thread priority
(default 0). The receiver's USB submission thread uses priority 1. USB interrupts
and cooperative workqueues still take precedence, and radio TX waits yield the CPU.
After startup, a fresh clock acknowledgement also allows waiting input to
proceed in the next input window. It does not advance an input sequence or
change the idle keepalive interval. If the first queued report was already
sent, it allows an earlier retry. `QUEUE clock_ack_advances` counts these
scheduling decisions. Replayed packets and repeated acknowledgements of the
same or older clock packet cannot trigger them.

The keyboard's `CONFIG_APEX_RADIO_KEYBOARD_EVENT_RX` routes RADIO DISABLED through
PPI channel 19 to EGU3/SWI3, waking the radio thread without waiting
for its polling timeout. Bluetooth's direct RADIO interrupt remains untouched.
The DISABLED event route is armed only after Bluetooth shutdown. EGU3 must not be used
by another driver. `RX_WAKE irq_to_thread_max_us` measures ISR-to-thread delay,
not packet airtime, USB transfer time or physical-key latency.

Receiver USB completion wakes the radio owner thread to update delivery counters.
The next solicited reply carries the completion position. Failed transfers and
old-session completions cannot advance it. Lost acknowledgements recover through
input retries or keepalives. Replies still use the input window.

A lost connection starts a fresh authenticated session. Input queues discard
old transitions and send the current state. A forced receiver-session reset
and a 500 ms USB submission stall recovered on hardware. One receiver clock
timeout shortly after startup also recovered, but its cause is unresolved.
Typing was also tested with keyboard USB disconnected. Longer runs,
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

The 7 September transaction builds delivered 6000 empty keyboard reports in six
1000-report runs at 631–643 reports/s. Each receiver USB completion was counted.
There were no new link timeouts within a run, TX wait timeouts or USB errors.
A deliberate receiver session reset and 500 ms USB hold between batches recovered.
That reset causes an expected peer timeout; it does not test a continuously full
receiver FIFO. Queue saturation, lost replies and ordered delivery are also
covered by host tests. These rates are throughput, not a 1 ms latency claim.

The earlier one-ahead input build measured 363–405 reports/s. The figures below
record earlier implementation stages and should not be read as current rates.

### Delivery throughput

For a keyboard benchmark build, append
`--extra-conf apex-zmk-g4b/g4b_radio_bench.conf` to the radio-input build command.
Leave keyboard USB connected and its switch in dongle mode. Once the link
has settled, run `apex radio_bench` in the keyboard shell.

An optional argument changes the clock transmission lead for that run:
`apex radio_bench 500` selects 500 us, also the transport's current default. Accepted
values are 500–2000 us. The previous value is restored when the test ends,
including on failure. This changes neither the channel slot nor its guards.
It lets us compare clock delays without flashing a build for each value.
With no argument the benchmark retains its original 2000 us comparison setting.

In earlier same-build tests, a 500 us lead delivered 247–299 reports/s across six
1000-report runs, without missed clock transmissions or new link timeouts.
The 2000 us baseline delivered 180–250 reports/s in five completed runs.
Another baseline run stopped after a link timeout. A 1000 us lead delivered
202–273 reports/s in three runs. The shorter lead looks useful, but these
tests did not establish long-run reliability. These results predate the current
buffered delivery and timer-driven retries.

This sends 1000 empty keyboard reports through the normal radio, receiver USB
and acknowledgement path. It bypasses unchanged-state filtering and uses the
normal 32-entry keyboard queue. Earlier benchmarks limited the generator to
eight entries; compare those results with that difference in mind.
Reports contain no pressed keys. The command stops
after ten seconds or an endpoint/session change. It requires working direct
USB output so generated reports do not replace normal wireless typing.

`BENCH` reports sent/acknowledged counts, elapsed microseconds and average
reports per second. Time includes queue filling and observing the final ACK.
This measures throughput under load, not a 1 ms cadence or physical-key latency.
Compare receiver USB counters before and after each test. The benchmark option
is disabled by default.

`apex radio_bench 500 paced` requests one report per millisecond instead of
filling the queue immediately. `BENCH_SOURCE late_max_us` reports how far the
generator fell behind that schedule, including thread delays and backpressure.
It uses the kernel cycle clock, so this is not a hardware-timed 1 kHz source.
The command waits for all USB completions, including reports queued near the end.

`USB_INTERVAL` records time between successive successful receiver USB input
completions in the same session. It includes idle gaps between tests and uses
the kernel clock's resolution. Compare snapshots and bucket counts rather than
treating its lifetime maximum as active typing latency. It does not timestamp
physical key presses or host application processing.

On the development keyboard and receiver, three runs with 20 ms channel slots
and the 4 ms hop gap delivered 1000 reports in 4.89, 4.54 and 4.26 seconds
(204, 220 and 234 reports/s). Receiver counters confirmed all 3000 USB
completions, with no transfer errors or new link timeouts during the runs.
USB submission-to-completion averaged 520 us across those reports. These
results are from USB-powered testing with the timer-wake build, not battery
or interference tests. They fall well short of the 1000 reports/s target.

### Timing counters

The older single-report timing counters below were collected before input
format 2. `SEND_ACK` tracks only one report and cannot represent all reports
with delivery pipelined. `COMPLETE_ACK` and `ACK completion_tx` no longer advance
because unsolicited completion replies were removed. Use `QUEUE_ACK` and USB
completion counts for the current pipeline until per-report timing is updated.

`SEND_ACK` measures from entering the first successful input send call to
processing its authenticated delivery ACK. It includes retries, USB delivery
and ACK handling. Failed or deferred send attempts do not start the measurement.
`ACK_NEXT` measures from processing that ACK to entering the next successful
input send call, only when another report was already queued at ACK time.
Session changes discard unfinished measurements.

`COMPLETE_ACK` measures from USB completion to the end of the receiver's
immediate completion ACK transmission. ACKs recovered through later input
retries are not included. The three counters are separate intervals with
different sample counts. Subtract counts and totals between snapshots before
calculating averages. Neither endpoint timestamp measures host application use.

`INPUT_ENCODE` times input preparation at the send site: either copying a
prepared packet or encoding one there, including retries and packets later
deferred by a channel guard. It excludes work done earlier by the preparation
helper, so a lower value does not mean encryption itself became faster.
`DATA_DECODE` times ordinary data-packet decoding
(input, ACK and keepalive), including rejected attempts. `ACK_ENCODE` times
delivery and liveness ACK preparation at the send site, excluding any earlier
prepared-ACK encryption. Clock and pairing packets are excluded.
These measure elapsed time around the calls, including any preemption.

A further three-run test with these counters delivered 189–212 reports/s.
Average `SEND_ACK` was 3.80 ms and `ACK_NEXT` was 1.14 ms. Input encoding
averaged 159 us, keyboard data decoding 166 us, receiver data decoding 184 us
and ACK encoding 167 us. USB completion averaged 536 us. All 3000 reports
completed without USB errors or new link timeouts. This points to delivery
and scheduling delays as the larger problem, rather than encryption alone.
It is not a controlled performance comparison with the earlier build.

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
