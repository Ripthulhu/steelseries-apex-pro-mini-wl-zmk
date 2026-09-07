# The 2.4 GHz receiver

The first-generation Apex Pro Mini Wireless uses an nRF52833 in both the
keyboard and USB receiver. The receiver has direct USB, a crystal and antenna,
but no button, LED or external flash.

The custom link carries keyboard and media reports, with encrypted sessions,
channel hopping and USB pairing. Typing through it has been confirmed on hardware.
It doesn't pair with GG or a receiver still running stock firmware. The older
keyboard v0.1.4 release predates this work. See the
[receiver guide](../../dongle/README.md) for matching builds and pairing.

## Why the early tests didn't settle the protocol

The vendor images contain several radio configurations. Finding an ESB-like
routine didn't prove it was selected for our handshake. Early attempts mixed
parameters from different paths, including normal Bluetooth operation.

A receive test caught nearby Bluetooth advertisements. That proved reception
with that configuration, not that proprietary transmissions matched the dongle's
framing, CRC, whitening or timing. The old conclusion that this ruled out a
radio-configuration fault was wrong.

Likewise, a search for register literals missed code that formed addresses and
values differently. The resulting claim that the link had no encryption was
later disproved by captured CCM operations.

## What the stock-protocol work established

The stock link uses the Nordic radio directly, with its own pairing and timing
code. It is not a normal Bluetooth connection.

Earlier notes said the link was unencrypted. That was wrong. Captured keyboard
CCM input, configuration and output reproduce AES-128-CCM encryption exactly in
an independent offline test. The capture used a zero key and IV and a four-byte
tag. This establishes that captured operation, not the key choice of every
keyboard or a working receiver handshake.

Pairing and usable input turned out to be separate milestones. GG reported
successful pairing in later tests, but the keyboard kept indicating pairing
mode and its keypresses did not reach the computer. Forcing completion flags
did not demonstrate a successful operational handshake.

The later captures and disassembly also corrected several early assumptions:

- The A0/90/90 exchange carries clear counter-bootstrap records. They are not
  the encrypted payload originally assumed.
- The first private B0 is the relevant encrypted handoff packet. Its live
  counter and phase were not verified.
- Pairing starts with CRCINIT 1, then changes to a value derived from the
  negotiated network ID. The earlier fixed 0xffffff pairing recipe was wrong.
- Stock turnaround uses TIMER2 and PPI. Halting the CPU at a breakpoint or
  replacing that timing with a software delay can invalidate a radio test.

The stock probes were removed from the keyboard source. Their prior versions
remain in local history and the capture notes remain useful for RE. The old proposal
that required an SDR is not a prerequisite for implementing our own protocol
now that we control both devices.

### The private-link handoff

The later captured pairing path used BLE 2 Mbit mode, big-endian framing,
whitening with IV `0x40`, and `BASE0=0x89BED600`, `PREFIX0=0x8E`. Its initial
CRC value was `1`. These findings supersede the early unwhitened, fixed-CRC
recipe. They aren't a complete pairing implementation.

The receiver sends a fragmented pairing record before the encrypted handoff.
In the investigated private link, receiver-to-keyboard packets used
`BASE0=0x76412900`, `PREFIX0=0x71` and 38-byte packets. The other direction used
the public-address register pair and 19-byte packets. The captured channel pair
was 76 and 2. Those observations shouldn't become a universal channel plan.

The counter scheme under investigation was `(slot << 1) | phase`, with a
1,600 microsecond slot. Direction distinguishes the peers. Correct AES with
the wrong slot or phase still fails authentication.

The clear `A0/90/90` records assemble the slot counter. Treating those `90`
packets as ciphertext sent the analysis in the wrong direction. The first
private `B0` remained the unresolved encrypted handoff, with the stock CCM
receive setup near keyboard address `0x3DDB6` the relevant code to trace.

The on-air packet isn't interchangeable with the Nordic CCM input buffer.
The peripheral expects its header, length and reserved-byte arrangement. The
offline encryption match didn't independently validate the first live receive
buffer, counter and phase together.

Breakpoints disturbed the experiment because stock uses TIMER2 and PPI to keep
radio events aligned. Halting across slots changes the counter and acknowledgement
state being inspected. A non-halting RAM trace was a better next experiment
than patching a guessed counter into memory. A stock operational link was never
demonstrated.

## The custom link

ZMK stays on the keyboard and produces the same reports it uses for USB.
The receiver forwards those reports instead of interpreting physical
scanner positions or maintaining a second keymap.

Both sides use a new, versioned protocol and a unique pairing key installed
over USB. Stock firmware and GG compatibility are not goals.

The shared packet implementation currently provides:

- AES-128-CCM with an eight-byte authentication tag;
- separate directional nonces and session keys derived from both peers' nonces;
- authenticated packet headers, bounded lengths and replay rejection;
- rejection of counter exhaustion instead of wrapping and reusing a nonce.

The authenticated handshake passes compiled ARM tests, including dropped-message
retries and rejection of old handshakes without resetting active counters.
The first fixed-channel tests established encrypted communication and then
keyboard/media input, using fresh hardware-generated nonces. The input builds
now exchange authenticated traffic across four channels. See
[the shared protocol](../../radio/README.md) for current packet formats, timing
windows, queue limits and reconnection behaviour.

The first live test detected the expected radio address and packet header but
failed CRC validation on every packet. Setting `CRCCNF.SKIPADDR` on both radios
resolved it. The protocol now explicitly excludes the address from hardware
CRC coverage. This is an observed fix; the underlying include-address mismatch
has not been fully explained.

The receiver uses the Nordic ECB accelerator for AES blocks. The same CCM code
is checked against independent reference vectors and on the device. The keyboard
also uses hardware AES after Bluetooth shuts down; Bluetooth-mode boots retain
software AES so the two stacks cannot use the peripheral at the same time.

See [the receiver application](../../dongle/README.md) for the build and shell
instructions. The release keyboard image retains the v0.1.4 settings and storage
layout. Its bootloader now handles the wireless-update promote and copy, and
programs each destination word once instead of reprogramming it up to 16 times
between erases.

## Why a 1 kHz timer wasn't enough

A report had to be encoded, transmitted, accepted, submitted to USB and
acknowledged. Waiting for that entire chain before preparing the next report
left time unused. Software wakeups and guarded channel changes added more gaps.

Separating receiver acceptance from USB completion let the keyboard send more
work while retaining each report until completion. Replies carry both positions
and free queue space. Retries don't replay already-delivered keys.

When reports are already queued, one packet carries up to three consecutive
reports. A short press and release remain two transitions. A lone report isn't
delayed to fill a batch. This is aggregation, not a general sliding window of
independently outstanding radio packets.

Both devices request the high-frequency crystal before starting the radio.
Live `HFCLKSTAT=0x00010001` confirmed a running crystal. TIMER2 divides that
clock to one-microsecond ticks. The kernel's low-frequency RC clock isn't the
radio timing reference.

Hardware compare events and PPI schedule transmission and capture radio END.
Receive is rearmed promptly after TX instead of waiting for a thread to run.
A short interrupt-side wait for DISABLED caused failures after USB reconnection.
Using the actual peripheral event replaced that timing assumption.

Thread priorities and wake conditions also mattered. The queue could contain
sendable reports even when earlier ones hadn't completed USB. Waiting for an
entirely empty queue missed that opportunity.

The 20 ms channel slots retain guards so an exchange can finish before hopping.
Shrinking the gaps helped. Removing them would allow a reply to arrive after
the other device had already changed channel.

## Measured result and remaining work

Later batched-input tests delivered roughly 889 to 954 reports/s. All 9,000
reports completed across the recorded nine runs. A separate receiver-only USB
test reached 997 to 999 reports/s. A subsequent GitHub-built receiver test
delivered all 3,000 reports at 909, 900 and 910 reports/s.

These were synthetic report tests. The paced producer could itself fall behind,
and completion counters don't timestamp physical actuation or host application
processing. They show throughput near the target, not sustained 1,000 Hz input
or proven 1 ms end-to-end latency. Real typing was also checked repeatedly with
keyboard USB unplugged.

The receiver stays present on USB when the keyboard disappears. Its active-link
timeout releases input after 100 ms. A new session clears stale queues and
resends current state. Disconnect tests checked that held input didn't remain
stuck on the host.

Wireless idle scanning, RGB shutdown and Nordic deep sleep were carried over
from Bluetooth. A shortened sleep test confirmed held-key wake and switching
between Bluetooth and dongle mode. Battery-current, interference and host-suspend
measurements remain separate work.

Keyboard and media forwarding don't establish full USB feature parity. Studio
and analogue controller forwarding remain further work. The proposed two-image external-flash migration isn't installed.
Keyboard bootloader updates and emergency recovery remain wired.
