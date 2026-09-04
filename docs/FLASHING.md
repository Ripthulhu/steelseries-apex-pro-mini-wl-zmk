# Updating and recovery

This page is for a keyboard that already runs this firmware. For an untouched
keyboard, start with [First installation](INSTALL.md).

The keyboard has a firmware-update mode that appears on your computer as a USB
drive named `APEXBOOT`. Firmware tools often call this mode DFU, short for
Device Firmware Update.

## Normal update

Download the recommended `apex-pro-mini-wl-ab.uf2` from the
[latest release](https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk/releases/latest).
If you downloaded the complete ZIP instead, use the `apex-zmk.uf2` inside it.
A UF2 file packages the firmware so it can be copied to the keyboard
like a normal file.

1. Connect the keyboard to the computer with a USB data cable.
2. Hold `Fn` + `Right Ctrl` + `Esc`.
3. Wait for the `APEXBOOT` drive to appear.
4. Copy the UF2 file you downloaded to `APEXBOOT`. If you are using the complete
   ZIP, copy its `apex-zmk.uf2` file.
5. Wait while the drive disconnects and the keyboard restarts.

Do not repeat the first-install erase for an ordinary update. The normal
application UF2 cannot overwrite the bootloader.

If `APEXBOOT` does not appear, check that the cable carries data, reconnect the
keyboard, and try the key combination again.

The drive also has two small diagnostic files. `INFO_UF2.TXT` shows why the
keyboard reset and whether A/B recovery is armed. `CRASH.BIN` contains the most
recent saved crash, when one exists. Neither file is needed for an update.

## Updating APEXBOOT

Most releases do not require a bootloader update. When one does, download
`apex-pro-mini-wl-bootloader-update.uf2` from that release. This file is only
for a first-generation Apex Pro Mini Wireless that already has this project's
`APEXBOOT` bootloader. It is not a way to install the project on a stock
keyboard.

1. Open `APEXBOOT` with `Fn` + `Right Ctrl` + `Esc`.
2. Copy `apex-pro-mini-wl-bootloader-update.uf2` to the drive and wait for the
   keyboard to restart.
3. Open `APEXBOOT` again and install `apex-pro-mini-wl-ab.uf2` from the same
   release.

The new bootloader is received in unused space above the application. It is not
copied over the running bootloader until every UF2 block has arrived and its
board and layout data have been checked. The release build also refuses to
ship an application that reaches into this staging space.

## Other ways to open update mode

These are intended for development or recovery. Most users only need the key
combination above.

### Through the serial port

While the keyboard is running, it exposes a small USB serial port for update
requests. Send the exact text `APEXDFU!` to that port and the keyboard will
restart as `APEXBOOT`. The port is usually named `/dev/ttyACM*`,
`/dev/tty.usbmodem*`, or `COM*`, depending on the computer.

Some Adafruit and Arduino tools request the same restart by changing that port
to 1200 baud. This is the mechanism sometimes called a “1200-baud touch”; it
does not send firmware and is only a signal to restart into update mode.

### With a hardware programmer

A programmer connected to the pads under the space bar can install an
application directly or force the bootloader to start. This is mainly useful
when `APEXBOOT` no longer appears. See
[Repairing a keyboard with a programmer](SWD_RECOVERY.md) for wiring and the
full recovery procedure. The installation guide lists the config filename for
each programmer and says whether OpenOCD runs on the Pi or on a laptop or
desktop. A Pico is only the USB-to-SWD probe; it does not run the flashing
commands.

## If the keyboard starts but keys do not work

Unplug it completely, wait a few seconds, and reconnect it. The application
normally restarts the STM32 key scanner itself, but a full power cycle can
recover it after an unusual reset.
