# Wireless receiver

Custom receiver firmware for the first-generation SteelSeries Apex Pro Mini
Wireless. It receives keyboard and media reports from ZMK over an encrypted
2.4 GHz link and forwards them over USB. The keymap stays on the keyboard.
It does not use the SteelSeries protocol or pair through GG.

This is development firmware. Published keyboard releases do not include this
radio protocol; both devices need matching builds from this checkout. Gamepad
output, Studio and wireless updates are not implemented on the receiver.

## Before flashing

The receiver needs our Adafruit-based bootloader, which exposes an
**APEXDONGLE** USB drive. This is separate from the keyboard's **APEXBOOT** drive.
Do not use keyboard firmware or the keyboard USB installer on the receiver.

Installing the custom receiver bootloader through the stock USB updater worked
on the development dongle without SWD. That installer and the reviewed
bootloader build are still local development files, not included in this repo
or the release downloads. The provisional `bootloader/apex_dongle_wl` files
are not the installed port. If your dongle still runs stock firmware, stop here;
the instructions below update an already-converted receiver.

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

The receiver has no button. The reviewed recovery bootloader offers a two-second
USB window at power-on; the original provisional loader does not support this.
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

Reports stay queued until acknowledged after USB submission. Retries use fresh
encrypted packet counters. A new session discards old transitions and sends the
current state. Replies carry host lock-key LED state back to ZMK.

The current schedule uses 20 ms channel slots and a 5 ms input retry interval
inside guarded transmit windows. It is not the planned 1 kHz scheduler, and
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
```

`dongle hid_test_hold` is a test command: it stalls USB report submission for
500 ms and restarts the radio session. `dongle crypto_test` checks encryption
locally. Neither measures end-to-end input latency.

The older `--radio-probe` build is a fixed-channel, keepalive-only experiment.
It requires `g4b_radio_probe.conf` on the keyboard and does not carry keypresses.
Do not mix it with the input builds above.
