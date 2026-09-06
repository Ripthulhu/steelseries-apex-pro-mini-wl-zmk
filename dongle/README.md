# Apex receiver firmware

This is the custom USB receiver for the first-generation Apex Pro Mini Wireless.
It currently provides a USB shell, watchdog recovery, USB pairing storage and
packet crypto tests.
**It does not receive keyboard input yet.** The keyboard can stay on v0.1.4
while the receiver is developed.

## Build

Use the pinned workspace created by the main project's setup instructions.
Run these commands on your computer, from the repository directory:

```sh
python tools/build_dongle.py --workspace ../work/zmk-upstream --output ../work/apex-receiver
```

The command prints the artifact directory. It contains the receiver UF2, HEX,
ELF, exact configuration, source/dependency hashes and build log. It does not
flash a device. Builds use separate, source-identified artifact directories.
The receiver uses the keyboard project's Zephyr revision and records the
workspace's USB-driver patches rather than silently ignoring them.

## Flash and open the shell

These instructions assume the **custom receiver bootloader is already
installed**. Do not run the stock keyboard installer against the dongle.

1. Enter the receiver bootloader. From a running receiver shell, use
   `dongle dfu`. From a hung application, follow the recovery instructions below.
2. Copy `apex-receiver.uf2` from the printed artifact directory to **APEXDONGLE**.
   Do not use the keyboard's UF2 file.
3. Open the receiver's new serial port at 115200 baud. Enable DTR if your serial
   terminal has that setting. Press Enter to get the `dongle:~$` prompt.

The bootloader uses USB `1d50:6170`; the application uses `1d50:6171`. These are
local development IDs, not a registered allocation for distribution. The serial
number stays the same, but the operating system may assign a different port.
For example, the development receiver uses COM17 in the bootloader and COM18
in the application. Linux normally exposes these as `/dev/ttyACM*`.

```text
dongle status       Firmware build, uptime, reset reason and watchdog
dongle crypto_test  Local encryption/reference/replay checks; no radio traffic
dongle dfu          Reboot this receiver into APEXDONGLE
help                Available Zephyr shell commands
```

The keyboard keeps its existing `apex` commands. A receiver command never
implicitly resets or changes the keyboard. Remote keyboard commands will use
an explicit target when that transport is implemented.

## Recover a hung receiver

This requires the recovery bootloader built from reviewed source
`751c7e74ddd6` or a later compatible version. The initial custom loader
`4dd61de19ee4` does not have this startup window.

Install pyserial in your Python environment, then run:

```sh
python tools/dongle.py hold-bootloader --timeout 60
```

While it waits, unplug and reconnect **the dongle**. The utility opens its serial
port during the two-second bootloader window and holds it in recovery. It does
not write flash. APEXDONGLE then remains available for a replacement UF2.
For multiple receivers, add `--serial SERIAL_NUMBER` before `hold-bootloader`.

A watchdog reset enters recovery automatically. Normal USB enumeration alone
does not hold the bootloader open. The receiver application also sends fatal
kernel errors to the bootloader. Its five-second watchdog runs while the CPU
sleeps; it pauses when halted by a debugger.

## Tests

The host-side packet tests run compiled ARM code and compare it with Python's
AESCCM implementation. They require `unicorn`, `pyelftools` and `cryptography`:

```sh
python radio/tests/test_packet.py --workspace ../work/zmk-upstream --output ../work/packet-tests
```

The on-device test measures encryption, decryption and replay checking together.
It is not a radio latency or battery-life measurement. On the development
receiver, 100 checks took about 402 ms with software AES and 52 ms with hardware
AES. The first encrypted packet also matches an independent reference vector.

## USB pairing development

`dongle pair status` shows a public pairing ID and key fingerprint, never the
key. The host utility transfers the key as a binary record through USB; do not
type keys into the shell. `dongle pair clear confirm` removes a pairing.
Pairing occupies the reserved flash pages at `0x6d000..0x74000` and survives
ordinary application updates.

If status reports error `-45`, those pages do not contain a readable NVS store.
After backing them up, `dongle pair_storage_init confirm` erases only those
28 KiB and initializes storage. It does not erase a readable store or touch
the application and bootloader.

The receiver has passed provisioning, invalid-record rejection, timeout,
replacement, clear and persistence tests across a UF2 update. Keyboard builds
with `CONFIG_APEX_G4B_RADIO_PAIRING=y` provide the matching `apex pair` command.
With both devices connected by USB, use their shell ports:

```sh
python tools/dongle.py pair --keyboard-port KEYBOARD_PORT --dongle-port RECEIVER_PORT
```

Use `--replace` only to replace an existing pairing. If one device was updated
before the other disconnected, reconnect both and repeat with `--replace`.
No key is printed or placed in the shell history. Pairing has been tested on
both devices and survives a keyboard restart.

## Radio hardware test

The optional `--radio-probe` receiver build exchanges encrypted test packets
with a keyboard built using `g4b_radio_probe.conf`. Keep USB connected for
diagnostics and select the keyboard's dongle position. Switching between
Bluetooth and dongle restarts the keyboard to change radio ownership.

`apex radio_test` and `dongle radio_test` show the test counters. State 5 means
the authenticated handshake completed. The `auth` counter counts received
authenticated test packets; `replay` counts a second local decode correctly
rejecting each packet. It is not an over-the-air replay injection test.

This uses Nordic 2 Mbit mode on 2440 MHz at 10 exchanges per second. The hardware
CRC covers the packet, excluding the radio address. It has completed a live
handshake and encrypted round trips between the two boards. It does not carry
keypresses, use channel hopping or implement the planned idle power policy.
Restart both devices after changing a pairing; this test loads its key at start.
Do not use it as a battery-life or 1 kHz latency benchmark.

## Still to implement

The production radio scheduler, channel hopping, gamepad forwarding,
Studio/settings forwarding and wireless keyboard updates.
The keyboard's current A/B layout has not been migrated.

## Keyboard input development

Build the receiver with `--radio-input` and the keyboard with
`g4b_radio_input.conf`. These are matching development builds, not compatible
with the older keepalive-only test above. Both devices must already be paired.

The keyboard sends ZMK's keyboard and media reports in order. Each report stays
queued until acknowledged; retries keep the report sequence number but use a
fresh encrypted packet counter. A new session discards old queued transitions
and sends the current state. Queue overflow also starts a new session.
While USB is busy, the receiver replies with the last completed sequence. This
keeps the radio connection alive without dropping the pending report.

Windows recognizes the receiver's keyboard and media interfaces. The first
empty reports have completed USB submission and been acknowledged over radio.
Typing through the dongle has been confirmed on hardware with the keyboard's
USB cable unplugged. Volume controls and release/reconnect after switching off
while holding a key also passed. Windows captures confirm that direct USB sends
the correct play/pause and previous/next track reports, although the user's
playback application did not respond. Host suspend still needs testing.
Lock-key LED state is included in replies and passed to ZMK's indicator
handler; this does not add an RGB indication by itself.

A working direct USB connection takes priority. Unplug the keyboard's USB cable
to test input through the dongle. The receiver remains a USB device if the radio
link disappears and queues release reports after a 100 ms link timeout.
USB cancellation and suspend/resume handling still need review before this is
ready for everyday use.

This version sends every 5 ms on one fixed channel. It is not the planned
1 kHz implementation or a battery-life benchmark.
`CONFIG_APEX_RADIO_TEST_CHANNEL` selects the frequency in MHz above 2400 and
must match on both devices. The input test configurations currently select
74 (2474 MHz) for comparison with the original 2440 MHz channel. This is a
development setting, not automatic channel selection.

Both devices use the Nordic AES peripheral. On the keyboard it is enabled
only after Bluetooth shuts down; Bluetooth-mode boots retain software AES.
`radio_test` shows hardware AES and fallback counts, delivered reports, USB
waits and link timeouts. `dongle hid_status` shows USB readiness, report counts
and the last nonzero media usage, without exposing typed characters.
Intermittent link timeouts are still under investigation.
`TIMEOUT_ACTIVITY` records transmitted packets, CRC-valid received packets,
CRC errors and rejected packets since the last accepted packet before a
timeout. These are activity counters, not an over-the-air capture; they cannot
identify an interfering transmitter. A receiver count of one transmitted
packet can be the reply to its last accepted packet.
The receiver can wake its radio thread on packet completion with
`CONFIG_APEX_RECEIVER_RADIO_EVENT_RX`; the input configuration enables this.
The keyboard still polls, pending a separate Bluetooth interrupt handover.
`dongle hid_test_hold` deliberately delays HID submission for 500 ms and
restarts the radio session. It is a diagnostic command, not a recovery step.
