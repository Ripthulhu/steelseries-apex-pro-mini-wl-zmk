# Stock USB updates and bootloaders

GG can replace application firmware over USB. We used that update path to
investigate the devices and, later, install a custom bootloader on the dongle.
The keyboard and receiver use different update protocols and have different
recovery constraints. A result on one doesn't establish the same result on
the other.

For installation commands, use the [keyboard guide](../INSTALL.md),
[experimental keyboard USB installer](../../installer/README.md), or
[dongle guide](../../dongle/INSTALL.md). This page records how the update paths
were recovered.

## Identifying the images

The GG 116 installer contains these 3.24.1 update images. Sizes and SHA-256
identify the exact files used in this investigation, independently of a renamed
working copy.

| Target | Bytes | Application base |
|---|---:|---|
| Keyboard Nordic | 307,200 | `0x1C000` |
| Keyboard STM32 | 51,200 | `0x08007000` |
| Dongle Nordic | 184,320 | `0x23000` |

```text
Keyboard Nordic
a351393392573bc5cba61b335f5f516f13a2d7fc6902860b65d379f90c9b6b98
Keyboard STM32
20820e6eb4d0d0e278c7a4968dff1ec078e872edfa1a8a47d34c082efd073956
Dongle Nordic
7ad309bc6bbb869118937718ee56323ca6158cbb47fa1745a9ed240e66af89fe
```

The receiver image's vector points into `0x23000`, not address zero. Some early
disassemblies used file-relative addresses. Add the image base before comparing
those offsets with a live program counter or a later addressed disassembly.

The installer is an NSIS archive that can be extracted with 7-Zip. The current
dongle installer downloads and checks it without installing or running GG.
The repo distributes our patches and code, not the extracted vendor firmware
or private device backups.

## GG's device descriptions

GG ships encrypted `.edevice` descriptions alongside firmware. These describe
the USB interfaces, file IDs and update sequence. The host executable contains
the material needed to decrypt them. The investigation checked the recovered
decryption by reproducing five already-decoded files byte for byte.

That revealed a useful distinction: encrypted host descriptions didn't mean
the MCU update images were encrypted or signed. A CRC checks transfer integrity.
It doesn't establish who produced an image.

For the keyboard, both normal `1038:1626` and recovery `1038:1627` instantiate
the same file-update definitions. The normal definition inherits read/write
file access from the common boilerplate. The sequence erases and writes before
its final reset, so the descriptor doesn't require a preliminary switch to the
recovery PID.

An early search through the host DLL found an "Enter Bootloader" flow belonging
to an Arctis headset. Applying that to the Apex was the wrong path. Likewise,
the third argument of `sync-interface` is an interface number, not a report
size. That interface carries device events, not firmware-command completion.
Neither finding explained the early normal-mode erase timeout.

## Keyboard staging

The keyboard update API targets Nordic file 11 on filesystem 3. The 307,200-byte
image is staged unchanged in external NOR at `0x014000`. Its complete CRC-32
has residue `0x2144DF1C`. The factory update path then applies it to the internal
application region at `0x1C000..0x67000`.

This allowed application experiments while retaining the existing loader.
It didn't provide a dump of that loader. Nor did a successful staged-file CRC
prove that arbitrary code would boot after reset. Those are separate checks.

The STM32 update is routed through the Nordic, with its own file operations.
Our installer doesn't update that processor because the stock application
image isn't a backup of its loader and calibration.

## Why the keyboard needed SWD

We couldn't obtain the protected keyboard bootloader. APPROTECT prevented SWD
reads, clearing protection required an erase, and the vendor update files didn't
contain it. The erase was needed to install a working open bootloader, not an
accidental loss of a backup opportunity.

Early notes called the entire low-flash region a SteelSeries bootloader. That
mixed the Nordic MBR and SoftDevice area with the vendor loader. It also led to
conflicting boot addresses. Use the current [bootloader reference](../BOOTLOADER.md)
and [flash map](../FLASH_MEMORY_MAP.md), rather than those early labels.

The open port uses Adafruit's nRF52 bootloader with this board's USB and RGB
setup. Removing the SoftDevice reservation lets Zephyr start at `0x1000`.
The MBR remains responsible for bootloader replacement when updating the loader
itself, so the running loader doesn't erase its own instructions.

There is still an experimental stock-to-`APEXBOOT` USB migration. Its offline
checks pass, and the MBR copy step was exercised from an SWD-started migration.
The full path starting with an untouched stock keyboard remains untested.
You can try it, but need working SWD recovery first.

## The receiver could install its own replacement

The dongle's stock USB updater accepted an unchanged application first, then
an application with a changed version string. That demonstrated that the tested
path accepted modified code, rather than merely accepting the vendor file.

A temporary application then exposed internal flash and UICR read-back over USB.
Two complete reads matched. Unlike the keyboard investigation, this gave us
the receiver's actual bootloader and configuration before migration.

The migration placed the new loader in high flash, verified it, prepared the
parameter/settings pages and changed the MBR page last. It preserved the
existing UICR configuration instead of guessing reset-pin or regulator settings.
The initial installation succeeded over USB without receiver SWD.

The corrected board port also removed guessed LED/button pins. A board with no
button needs software recovery that works even when its application doesn't.
The installed loader provides a two-second USB window after power-on, which
a host can hold open. Crash/watchdog resets return to the loader. The receiver
application also has a USB command to enter it.

## What was tested, and what wasn't

The development dongle passed USB installation of the custom loader, real
unplug/replug, UF2 application updates and serial application updates. Wrong-family
UF2 was rejected. Parser tests also run the compiled ARM code against malformed
inputs and injected migration failures.

The later one-command installer packages that work without a GG dependency.
Its preparation was checked on Windows and Linux, and its application stage
was tested on the already-converted receiver. The full packaged sequence hasn't
been rerun from a receiver restored to stock. Stock restoration itself hasn't
been tested, and the development receiver currently has USB access only.

These distinctions matter. The original USB migration worked, but that's not
the same claim as testing every subsequent packaging change on a fresh stock
dongle. The [dongle installation guide](../../dongle/INSTALL.md) keeps the current
test coverage and recovery instructions together.

One packaging test found a host-side error after a successful UF2 copy. The
receiver had rebooted and disappeared before the final filesystem sync, causing
an invalid-handle error. The installer now handles the expected disconnect only
after the complete write and flush, then requires the application to report
the expected build. It doesn't treat arbitrary copy errors as success.

## Code behind the current tools

- [Stock keyboard file-transfer client](../../installer/flash_stock.py)
- [Receiver USB update client](../../dongle/installer/usb_update.py)
- [Receiver read-back client](../../dongle/installer/readback.py)
- [Receiver migration code](../../dongle/installer/migrate.c)
- [Packaged receiver installer](../../dongle/installer/install.py)
- [Reviewed receiver bootloader patch](../../dongle/bootloader/port.patch)
- [Compiled-ARM migration tests](../../dongle/installer/test_migration_arm.py)

These are the maintained implementations. Earlier local scripts and disassembly
notes explain their development, but aren't alternative installation instructions.
