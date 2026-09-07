# Wireless receiver

Custom receiver firmware for the first-generation SteelSeries Apex Pro Mini
Wireless. It receives keyboard and media reports from ZMK over an encrypted
2.4 GHz link and forwards them over USB. The keymap stays on the keyboard.
It does not use the SteelSeries protocol or pair through GG.

This is development firmware. Both devices need matching radio builds; the
normal keyboard release UF2 does not include this protocol. Studio and wireless
updates are not implemented on the receiver.

## Analog controller

Press `Fn`+`Z` to turn the controller on or off. W/A/S/D provide the same axes
as [the keyboard's USB gamepad](../docs/FEATURES.md#analog-gamepad). The receiver
briefly reconnects its USB interfaces when the controller is added or removed.
Keyboard and media keys continue to use their usual interface.

A data connection to the keyboard's own USB port takes priority. Unplugging it
returns controller output to the dongle. A battery bank does not take priority.
Bluetooth does not support controller output.

This is a generic HID controller, not an Xbox controller. Games may need input
mapping or mods, and some will not work well with mixed keyboard/controller input.

## Before flashing

The receiver needs our Adafruit-based bootloader, which exposes an
**APEXDONGLE** USB drive. This is separate from the keyboard's **APEXBOOT** drive.
Do not use keyboard firmware or the keyboard USB installer on the receiver.

For a stock receiver, follow [Install the receiver firmware](INSTALL.md).
One command downloads and extracts the official firmware without installing GG,
saves a backup, converts the bootloader and installs the receiver application.
The original conversion worked over USB without SWD. The combined command has
not yet been tested end to end on an untouched stock receiver.

Ready-to-flash files are available under **Build keyboard and receiver** in
[GitHub Actions](https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk/actions/workflows/radio.yml).
Choose a successful run and download both `apex-dongle` and
`apex-keyboard-radio` from that same run. GitHub requires a login for these
development downloads. Tagged releases also publish the receiver ZIP and a
separate `apex-keyboard-radio.uf2`; the normal keyboard UF2 is a different build.

## Build both applications

Follow [Building from source](../docs/BUILDING.md#prepare-the-build-tools) to
install the pinned workspace. Run these commands on your computer, from the
repository root. Use `python3` instead of `python` if that is its name on your
system. The scripts select the workspace's Python environment themselves.

Receiver:

```sh
python tools/build_dongle.py --workspace ../work/zmk-upstream --output ../work/apex-receiver --radio-input
```

The command prints an artifact directory containing `apex-receiver.uf2`, its
exact configuration, ELF/HEX files, build log and `build.json` with SHA-256
hashes. It does not flash anything. Omitting `--radio-input` builds a USB console
without keyboard input.

To also build the recovery bootloader and stock installer ZIP:

```sh
python tools/build_dongle_bundle.py --workspace ../work/zmk-upstream --output ../work/apex-dongle-bundle
```

This produces `../work/apex-dongle-bundle/apex-dongle.zip`. The build does not
need GG or the stock firmware: it packages patches which the installer applies
to the separately downloaded, hash-checked stock image.

Keyboard (one command):

```sh
python apex-zmk-g4b/build_g4b.py --stage 3 --usb-studio --kscan-ingest --persistent --plain-image --wireless-idle --ab-rollback --shell --extra-conf apex-zmk-g4b/g4b_shell_release.conf --extra-conf apex-zmk-g4b/g4b_radio_input.conf --work-root ../work
```

Its update file is
`../work/artifacts-repo-apex-zmk-g4b-wireless-idle-ab-v2/apex-zmk-g4b.plain.uf2`.
Watchdog and A/B recovery remain enabled. These builds include diagnostic
shells; they are not a new production release.

## Install the applications

1. On the keyboard, hold Fn + Right Ctrl + Esc to open **APEXBOOT**, then copy
   the keyboard UF2 to it using your file manager. See the
   [keyboard update guide](../README.md#updating-an-installed-keyboard) for the normal update instructions.
2. In the receiver's serial shell, enter `dongle dfu`. If the application cannot
   open a shell, use [recovery](#recover-an-unresponsive-receiver) below.
3. Copy `apex-receiver.uf2` to **APEXDONGLE** using your file manager. Wait for
   the copy to finish. The drive disappears when the receiver restarts.

Use the same file-manager copy on Windows, Linux and macOS. Drive letters and
mount paths vary; identify each drive by its label. Application updates preserve
the receiver's pairing storage and do not replace its bootloader. Hardware tests
so far used Windows; Linux and macOS USB operation has not been verified here.

## Open a serial shell

Use a serial terminal at 115200 baud with DTR enabled. Press Enter to display
the prompt: `apex$` for the keyboard or `dongle:~$` for the receiver.
The keyboard has separate Studio and shell serial ports; use the shell port.

Python's pyserial package also provides a terminal. To keep its dependencies
separate from your system Python, create a virtual environment:

```sh
python -m venv .venv
```

For the commands in the rest of this page, replace `python` with
`.venv/Scripts/python.exe` on Windows or `.venv/bin/python` on Linux/macOS.
No shell activation is needed. Install pyserial, list the ports, then open one:

```sh
python -m pip install pyserial
python -m serial.tools.list_ports -v
python -m serial.tools.miniterm PORT 115200
```

Replace `PORT` with the actual port name: for example `COM5` on Windows,
`/dev/ttyACM0` on Linux or `/dev/cu.usbmodem...` on macOS. Exit miniterm with
Ctrl + ]. Close the terminal before running the pairing or recovery utility.
On Linux, your account needs permission to open the serial device; the group
used for this is often `dialout`, depending on the distribution.

| Device | USB ID |
| --- | --- |
| Keyboard application | `1d50:615e` |
| Receiver application | `1d50:6171` |
| Receiver bootloader | `1d50:6170` |

These are local development IDs, not registered allocations for distribution.
The USB serial number is stable, but the port name can change after an update.

## Keyboard diagnostics through the receiver

Open the receiver's serial shell as described above. With the keyboard awake
and its switch in dongle mode, enter:

```text
keyboard battery
keyboard radio
keyboard gamepad
```

`keyboard` addresses the keyboard; `dongle` addresses the receiver. Enter
`keyboard` by itself for a short command guide. These commands run the
keyboard's existing `apex` handlers over the radio. The keyboard
does not need a USB cable. Output starts with `[keyboard]` and ends with
`[keyboard result: 0]` on success. Commands without the `keyboard` prefix, such
as `dongle radio_test`, refer to the receiver itself.

This also supports settings commands, for example
`keyboard rt on 0.3`. Commands run once; a lost reply does not cause
them to run again. If the connection drops, the receiver reports that the command
may have run. Check its result before repeating a command that changes anything.
Rebooting the keyboard or switching it to Bluetooth will interrupt the reply.

Use the same command names and arguments as the keyboard's USB shell, replacing
`apex` with `keyboard`. `keyboard radio` is the shorter name for `apex radio_test`.
Only `apex` handlers are supported, with a 127-character internal command limit, 16 KiB
of output and a 15-second timeout. Use short snapshots instead of long monitors.
Pairing commands still require the keyboard's own USB shell.
This is not an interactive keyboard terminal or a live log stream. Typing takes
priority over diagnostic traffic. Both devices need shell-capable radio builds;
these are included in the radio build commands above.

## Pair over USB

Connect both devices by USB and close their serial terminals. Replace the two
port placeholders with their shell ports:

```sh
python tools/dongle.py pair --keyboard-port KEYBOARD_PORT --dongle-port RECEIVER_PORT
```

This installs a unique shared key without printing it or placing it in shell
history. Use `--replace` to replace an existing pairing, or to repeat an
interrupted pairing that updated only one device. Restart both devices afterward;
the radio loads its key at startup. Pairing survives normal firmware updates.

Set the keyboard switch to dongle mode and unplug its USB cable to use the
receiver. A working keyboard USB connection takes priority. A charge-only
source leaves the wireless mode selected. Switching between Bluetooth and
dongle mode restarts the keyboard to hand over the Nordic radio peripheral.

## Recover an unresponsive receiver

The receiver has no button. Its recovery bootloader offers a two-second
USB window at power-on.
With pyserial installed, run:

```sh
python tools/dongle.py hold-bootloader --timeout 60
```

While it waits, unplug and reconnect **only the receiver**. The utility holds
the bootloader open without writing flash. Copy a receiver UF2 to **APEXDONGLE**.
If several receivers are connected, add `--serial SERIAL_NUMBER` before
`hold-bootloader` to choose one. Watchdog and fatal application errors also
return to the bootloader.

## Keyboard input development

Typing, volume controls, key release after switching off, and reconnection have
been tested on hardware. Both devices receive authenticated packets on all four
channels, and typing with keyboard USB disconnected works with hopping enabled.
The receiver stays connected to USB when the keyboard disconnects and queues
release reports after a 100 ms link timeout. USB cancellation and host
suspend/resume handling still need testing.

Reports stay queued until the USB transfer completes successfully. Failed or
cancelled transfers are retried; `dongle hid_status` counts them as
`transfer_errors`. Retries use fresh
encrypted packet counters. A new session discards old transitions and sends the
current state. Replies carry host lock-key LED state back to ZMK.

Replies distinguish reports accepted into the receiver queue from reports
actually completed over USB. Up to three reports share one radio packet when
input is queued. USB completions are reported in the next solicited reply;
unsolicited completion replies are not used. See the [protocol notes](../radio/README.md)
for the delivery format and timing counters.

Recent sustained tests delivered 889–954 reports per second through radio and
997–999 through USB alone. All 9,000 reports in the latest radio batch completed.
These are throughput tests, not measurements from physical keypress to application.

On battery, dongle mode now shares the keyboard's Bluetooth idle policy: scanner
periods of 50 ms after five seconds and 255 ms after a minute, plus RGB shutdown
after 30 seconds. A key wakes the scanner thread through ATTN. USB power disables
scanner throttling; a charge-only source still permits the RGB timeout. The radio
itself still runs the active test schedule, so this does not provide Bluetooth's
radio power savings. Radio duty cycling remains to be implemented and measured.
`CONFIG_APEX_G4B_DONGLE_SLEEP` enables experimental Nordic System OFF after
`CONFIG_APEX_G4B_SLEEP_MS`. The radio-input configuration enables it with the
normal 15-minute timeout. This stops the radio and wakes through a full reboot,
not an ordinary idle scan. Key wake and switching to Bluetooth, then back to
dongle mode, passed hardware tests with a shortened 60-second timeout. The full
15-minute wait and current consumption have not yet been measured. The middle
USB switch position is not a guaranteed wake source.

The current schedule uses 20 ms channel slots, 1.5 ms input retries and 5 ms
idle keepalives inside guarded transmit windows. It is not a proven 1 kHz link, and
input latency and battery life have not been measured. An early clock timeout
recovered automatically but remains unexplained. See the
[protocol notes](../radio/README.md) for timing, tests and diagnostics.

Useful receiver shell commands:

```text
dongle status       Build, uptime, reset reason and watchdog
dongle radio_test   Connection, timing and delivery counters
dongle hid_status   USB readiness and report counters
dongle pair status  Pairing ID and key fingerprint, not the key
dongle dfu          Restart into APEXDONGLE
dongle reconnect    Start a fresh encrypted radio session
```

For keyboard application updates, see [wireless updates](../update/README.md).
The keyboard needs one-time wired preparation before it can accept them.

`dongle hid_test_hold` is a test command: it stalls USB report submission for
500 ms and restarts the radio session. `dongle crypto_test` checks encryption
locally. Neither measures end-to-end input latency.

`dongle hid_bench` sends 1,000 empty keyboard reports through USB and measures
completion rate without radio traffic. Use it with the keyboard connected
directly over USB: it briefly pauses the radio link and releases held input.
The link reconnects when the test finishes. This isolates USB throughput;
it does not measure wireless throughput or keypress latency.

The older `--radio-probe` build is a fixed-channel, keepalive-only experiment.
It requires `g4b_radio_probe.conf` on the keyboard and does not carry keypresses.
Do not mix it with the input builds above.

The HID completion regression test runs on a computer with Python and a C
compiler (`cc`, or select one with `--cc`):

```sh
python dongle/tests/test_hid_completion.py
```

It checks failed transfers, key-release retries and completions from an old
session. These tests do not replace USB suspend/resume tests on hardware.
