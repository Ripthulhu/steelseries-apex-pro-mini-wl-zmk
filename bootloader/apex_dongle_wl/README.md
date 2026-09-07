# Receiver bootloader board

Reviewed nRF52833 port for the first-generation Apex Pro Mini Wireless receiver.
It has direct USB, no LEDs and no user button. GPIO and voltage configuration
are left unchanged.

`tools/build_dongle_bundle.py` applies `dongle/bootloader/port.patch` to the
pinned Adafruit source and builds these board files. It does not use the
keyboard's bootloader patches, RGB code or external-flash recovery.

The bootloader provides the APEXDONGLE drive, receiver-specific UF2/serial image
checks, a two-second USB recovery window and recovery after watchdog/crash
resets. Applications start at `0x1000`; pairing storage at `0x6d000` is outside
the application update area. The loader is at `0x74000`, with MBR parameters
at `0x7e000` and settings at `0x7f000`.

For installation and recovery, see [the receiver guide](../../dongle/INSTALL.md).
