# Install the receiver firmware

For the **first-generation Apex Pro Mini Wireless dongle**, not the keyboard.
The installer replaces its stock bootloader and application over USB. It then
checks that the new receiver application starts. No compiler or GG installation
is needed. Commands run on the computer the dongle is plugged into.

The individual USB conversion, UF2 upload and recovery steps have worked on our
development receiver. The combined installer has not yet been run end to end
on an untouched stock receiver. Keep SWD recovery available: losing power while
the bootloader is being replaced can leave the dongle unable to start over USB.

## What you need

- `apex-dongle.zip` from the same GitHub build as your keyboard firmware.
- Python 3.11 or newer and [7-Zip](https://www.7-zip.org/).
  On Ubuntu/Debian, install `7zip`; on macOS with Homebrew, install `sevenzip`.
- Only one Apex dongle connected. Close GG if it happens to be running.

Extract the ZIP and open a terminal in that folder. Create a Python environment:

```sh
python -m venv .venv
```

For the remaining commands, use `.venv/bin/python` on Linux/macOS or
`.venv/Scripts/python.exe` on Windows in place of `python`.

```sh
python -m pip install hidapi pyserial
python install.py --confirm
```

The command downloads the fixed GG 116.0.0 archive from SteelSeries (403 MiB),
checks its SHA-256 and extracts just the receiver firmware with 7-Zip.
**It never installs or runs GG**, including on Linux and macOS. The archive
and extracted firmware are cached in `download-cache` for later use.

Before replacing the bootloader, it uploads temporary readback firmware and
saves two matching reads of the dongle's flash and Nordic configuration in
`dongle-backup`. These include the temporary application, not the application
that was installed before you started. Keep this backup somewhere safe.
Unexpected boot addresses or differing reads stop installation.

Leave the dongle connected until the command reports that the receiver build
is running. It installs the bootloader, waits for **APEXDONGLE**, and copies the
receiver application to it. Your desktop must mount USB drives automatically;
otherwise mount APEXDONGLE yourself when it appears. Use `--drive PATH` for a
mount location the installer cannot find.

You can download and check everything before connecting the dongle:

```sh
python install.py --prepare-only
```

For an offline install, copy `download-cache` from that computer. Alternatively,
use `--stock-image PATH` with the exact stock receiver 3.24.1 binary. Its hash
is checked; other firmware versions are rejected. Use `--7zip PATH` if 7-Zip
is not on your command path. Existing backup folders are never overwritten;
use `--backup another-folder` for another dongle.

Linux needs access to both the stock HID interface and the custom serial port.
The stock USB IDs are `1038:1624`/`1038:1625`; the custom IDs are
`1d50:6170` (bootloader) and `1d50:6171` (application). Add an appropriate udev
rule or grant device access for your account; serial access commonly uses the
`dialout` group. Do not replace a Windows USB driver with WinUSB: this installer
uses HID and the standard serial/mass-storage drivers.

## After installation

Pair the dongle and keyboard over USB using `dongle.py pair`; the full steps
are in the repository's [receiver guide](https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk/blob/apex-scanner-bootloader/dongle/README.md#pair-over-usb).
Both devices need matching radio firmware. Stock GG pairing no longer applies.

Future updates only need `apex-receiver.uf2`: enter `dongle dfu` in the receiver
shell and copy the UF2 to **APEXDONGLE**. Do not repeat stock installation.

## If installation stops

Keep the backup and `usb-update-*.jsonl` logs. A successful upload CRC checks
the temporary installer, not completion of bootloader replacement.

If APEXDONGLE appears, the custom bootloader is running. Copy `apex-receiver.uf2`
there manually; never copy a keyboard UF2. If the receiver application is hung,
run `python dongle.py hold-bootloader --timeout 60`, then unplug and reconnect
only the dongle. Copy the receiver UF2 when APEXDONGLE appears.

If neither stock USB nor APEXDONGLE returns, stop retrying. Recovery may need
SWD. A backup does not guarantee that USB recovery will remain available.
