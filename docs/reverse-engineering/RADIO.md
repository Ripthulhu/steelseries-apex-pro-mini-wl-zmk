# The 2.4 GHz receiver

The first-generation Apex Pro Mini Wireless uses an nRF52833 in both the
keyboard and USB receiver. The receiver has direct USB, a crystal and antenna,
but no button, LED or external flash.

The keyboard's v0.1.4 release does not support the dongle. The custom receiver
bootloader works over USB. A receiver application with a USB shell and packet
crypto tests and USB pairing storage is running on the development dongle.
An optional radio test has completed an authenticated handshake and encrypted
round trips with the keyboard. A newer development build adds keyboard and
media forwarding; typing through it has been confirmed on hardware. See the
[receiver development notes](../../dongle/README.md#keyboard-input-development).

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

## The custom link

ZMK stays on the keyboard and produces the same reports it uses for USB.
The input test receiver forwards those reports instead of interpreting physical
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
now exchange authenticated traffic across four channels. Their ordered report
queues use a 5 ms retry interval within the hopping transmit windows; the 1 kHz
scheduler is unfinished. See [the shared protocol notes](../../radio/README.md)
for current timing, reconnection results and the unresolved startup timeout.

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
instructions. The keyboard test build retains the v0.1.4 settings and storage
layout. Its bootloader has not changed.
