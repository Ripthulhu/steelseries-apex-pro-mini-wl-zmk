# Repairing a keyboard with a programmer

This page primarily covers the normal SWD-installed layout, with the open
bootloader at `0x74000` and debug access enabled. For a keyboard that still has
the SteelSeries firmware, start with [First installation](INSTALL.md).

The untested USB migration uses a transition layout at `0x6E000` and leaves the
factory UICR and debug protection unchanged. If that keyboard still shows
`APEXBOOT`, use a normal UF2 update. Do not use the bootloader/UICR repair below
on that layout. If its bootloader no longer runs, use
[Erase and reinstall the open firmware](#erase-and-reinstall-the-open-firmware)
to convert it to the normal layout.

The programming pads are the nine-pad `CN3` header under the space bar. Never
use the separate five-pad STM32 header. Unplug the keyboard before attaching or
removing probe wires, and power it through its own USB-C port while programming.
Do not connect a probe's 3.3 V or 5 V output.

## Prepare the programmer and files

If the programmer is not already set up and wired, follow
[Choose a programmer](INSTALL.md#choose-a-programmer), then run the
[connection check](INSTALL.md#test-the-wiring-without-erasing-anything). Return
here only after the expected Nordic identification numbers appear. Those
sections also explain which computer runs OpenOCD; a Pico is a USB probe and
does not run commands itself.

Download and extract `apex-pro-mini-wl-ab.zip` from the
[latest release](https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk/releases/latest)
on the computer running OpenOCD. Open a terminal in the extracted folder.

First check the download without changing the keyboard:

```sh
python3 verify_bundle.py .
```

On Windows, use `python` if that is the name installed on your system.

Every file must report `OK`.

The examples on this page use `pico-debugprobe.cfg`. Substitute the matching
config file from [Choose a programmer](INSTALL.md#choose-a-programmer) when using
another probe. Commands using Raspberry Pi GPIO also need `sudo` before
`openocd`.

## Try the least invasive repair first

Try these in order and stop as soon as the keyboard works:

1. Copy the ready-to-use UF2 file if the `APEXBOOT` drive still appears.
2. Ask the installed bootloader to open `APEXBOOT` through the probe.
3. Reflash only the application.
4. Repair the bootloader using a saved UICR backup.
5. Erase and reinstall the open firmware, or restore a complete matching backup.

The last two choices write more of the Nordic chip. None of these commands
erase the external SPI flash that holds settings, Bluetooth pairings, A/B
recovery data, and crash dumps.

## Force the APEXBOOT drive to appear

This writes the bootloader restart register and resets the Nordic:

```sh
openocd -f pico-debugprobe.cfg -f target/nordic/nrf52.cfg -c "adapter speed 400; init; mww 0x4000051C 0x57; reset run; shutdown"
```

The keyboard should appear as USB device `1D50:616F` with a drive named
`APEXBOOT`. Copy `apex-zmk.uf2` from the extracted folder to that drive.

## Reflash only the application

Use this when the bootloader works but the application does not:

```sh
openocd -f pico-debugprobe.cfg -f target/nordic/nrf52.cfg -c "adapter speed 400; init; reset halt; program apex-zmk.hex verify; reset run; shutdown"
```

OpenOCD writes `apex-zmk.hex`, reads it back, and restarts the keyboard. The
MBR, bootloader, UICR, and external SPI flash are unchanged.

## Make a backup before a larger repair

On the normal `0x74000` layout, save the complete internal flash and UICR before
a larger repair:

```sh
openocd -f pico-debugprobe.cfg -f target/nordic/nrf52.cfg -c "adapter speed 400; init; reset halt; dump_image flash-open.bin 0x00000000 0x80000; dump_image uicr-open-backup.bin 0x10001000 0x1000; reset run; shutdown"
```

Check that `flash-open.bin` is 524288 bytes and `uicr-open-backup.bin` is 4096
bytes. Keep both files together. The UICR file contains the boot address, reset
pin assignment, and debug settings.

Before writing that UICR file back, check its six required words:

```sh
python3 -c "from pathlib import Path; b=Path('uicr-open-backup.bin').read_bytes(); print(len(b), *('%08X' % int.from_bytes(b[o:o+4], 'little') for o in (0x14,0x18,0x200,0x204,0x208,0x20C)))"
```

On Windows, use `python` if needed. The exact output for the normal `0x74000`
layout is:

```text
4096 00074000 0007E000 00000012 00000012 FFFFFFFF FFFFFFFE
```

Do not restore a UICR file that prints anything else.

## Repair the bootloader in the normal layout

This needs `uicr-open-backup.bin` from the same keyboard after the open
bootloader was installed at `0x74000`. Do not use it on the experimental
`0x6E000` migration layout. The command rewrites the MBR and bootloader, clears
their metadata pages, restores UICR, and asks the bootloader to start. Run the
UICR check above first.

```sh
openocd -f pico-debugprobe.cfg -f target/nordic/nrf52.cfg -c "adapter speed 400; init; reset halt; program bootloader_mbr.hex verify; flash erase_sector 0 126 127; flash write_image erase uicr-open-backup.bin 0x10001000 bin; verify_image uicr-open-backup.bin 0x10001000 bin; mww 0x4000051C 0x57; reset run; shutdown"
```

When it finishes, `APEXBOOT` should appear. Copy `apex-zmk.uf2` to complete the
repair.

## Erase and reinstall the open firmware

Use this only when the smaller repairs are impossible. It erases Nordic flash
and UICR, then installs the current release:

```sh
openocd -f pico-debugprobe.cfg -f target/nordic/nrf52.cfg -c "adapter speed 400; init; nrf52_recover; reset halt; program bootloader_mbr.hex verify; program apex-zmk.hex verify; flash write_image erase uicr-open.bin 0x10001000 bin; verify_image uicr-open.bin 0x10001000 bin; reset run; shutdown"
```

Every write is read back and verified. If the command stops after erasing, fix
the reported problem and run the same command again; the keyboard remains
recoverable through the programming pads.

## Restore a complete backup

Restore only a flash and UICR dump made together. This erases the Nordic before
writing both files. Run the UICR check above first:

```sh
openocd -f pico-debugprobe.cfg -f target/nordic/nrf52.cfg -c "adapter speed 400; init; nrf52_recover; reset halt; flash write_image erase flash-open.bin 0x00000000 bin; verify_image flash-open.bin 0x00000000 bin; flash write_image erase uicr-open-backup.bin 0x10001000 bin; verify_image uicr-open-backup.bin 0x10001000 bin; reset run; shutdown"
```

A SteelSeries restore needs a factory flash image and matching UICR, which are
not currently available. See [Restoring stock](BOOTLOADER.md#restoring-stock).

## Check the result

Unplug the keyboard before removing the probe wires, then reconnect it normally.

- The application identifies as USB `1D50:615E`.
- The bootloader identifies as USB `1D50:616F` and provides `APEXBOOT`.
- `Fn` + `Right Ctrl` + `Esc` opens the bootloader.
- USB and Bluetooth both type normally.
- Settings survive a full power cycle.

If USB works but keys do not, unplug the keyboard completely for a few seconds
and reconnect it. This can resynchronize the STM32 scanner and is not a reason
to erase either processor.

## Manual reset

If USB works but the scanner remains unresponsive after a full power cycle,
briefly short CN3 `RESET` to `GND` once. Remove the short immediately. Two quick
pulses tell the bootloader to open `APEXBOOT` instead.
