# Wireless keyboard updates

**Experimental.** Changed-image installation now works on hardware, but the
feature has had limited testing; keep a wired recovery path available.
Bootloader updates and emergency recovery still need USB or SWD.

## Hardware results

- A complete 402,176-byte image transfers on battery in about 100–120 seconds
  (~3 KB/s over the radio). The earlier text-command transfer was abandoned
  because it took far too long.
- Reinstalling the same image, and installing a *different* image, both pass
  verification, the internal copy, reboot and the keyboard's boot-health checks
  (state HEALTHY). An earlier bootloader corrupted changed images during the
  copy; see below.
- Progress shows on the key matrix: a red bar fills left→right while an update
  transfers and again while the bootloader copies it into internal flash, then
  the whole board turns green when each step completes — the same indicator the
  UF2 DFU path uses.

### The changed-image copy defect (fixed)

An earlier bootloader flushed the Adafruit 4 KiB flash page cache after every
256-byte write, so `flash_nrf5x_flush(false)` reprogrammed the whole page each
time and each internal word was written up to sixteen times between erases —
beyond the nRF52 write-between-erase limit. The copy verified immediately but
did not retain across a reboot, so a changed image booted corrupt and was
rejected. Reinstalling the *same* image hid the defect because the installer
skips pages that already match. The installer now caches each page and programs
every word exactly once; `update/test_bootloader_flash.py` models the real
page-cache and NVMC write count and guards against a regression. No physical
interruption-during-install test has been completed.

## One-time setup

The keyboard needs the normal `0x74000` bootloader layout. The experimental
stock USB installer's smaller `0x6e000` layout is not supported here.

1. Back up the keyboard's complete internal and external flash before changing
   its bootloader or preparing storage. Keep these files private: they contain
   Bluetooth bonds and the radio pairing key.
   From the repository folder, with Python and pyserial installed, run:

   ```sh
   python tools/dongle.py backup --keyboard-port PORT --output /path/to/private-backup
   ```

   Use the keyboard's own USB shell port, not the receiver port. The output
   directory must not already exist. This reads both flash chips and writes a
   checksum manifest only when all reads finish; allow about 25 minutes.
2. Install the update-capable keyboard bootloader over USB. Its `INFO_UF2.TXT`
   must contain `APEX-BOOT-OTA3`. Install the bootloader **before** the new
   application: bootloader self-updates use part of the application space as
   temporary storage.
   With keyboard USB connected, press **Fn + right Ctrl + Esc** to open
   `APEXBOOT`. Copy `bootloader_mbr.uf2` onto that drive. Wait for it to
   restart, then check `INFO_UF2.TXT` on `APEXBOOT`. If the keyboard returns
   to normal typing instead, use the same key combination to reopen the drive.
3. Install a keyboard build with `CONFIG_APEX_G4B_WIRELESS_UPDATE=y` and a
   receiver build with `dongle reconnect` and `keyboard` shell commands.
   Copy the keyboard application's `.uf2` onto `APEXBOOT`. Update the receiver
   through its separate `APEXDONGLE` drive as described in the
   [receiver instructions](../dongle/README.md).
4. Open the keyboard's USB shell and run:

   ```text
   apex update status
   apex update prepare backup-confirmed
   apex update status
   ```

   Wait for `prepared=1 bootloader=1 ready=1` and `bulk=1`. `ready` means the keyboard has
   passed its usual boot checks and verified its fallback image.

Preparation clears the old, unused LittleFS reservation. It accepts only erased
flash or the exact empty filesystem produced by the pinned formatter. If it
finds other contents, it stops. Settings, bonds, the fallback image and crash
dumps keep their existing addresses. An interrupted preparation can be repeated
over USB.

## Send an update

Run these commands on the computer connected to the receiver, from the repository
folder. Python and pyserial are required (`python -m pip install pyserial`).
Replace `PORT` with the receiver's shell port, such as `COM18` on Windows or
`/dev/ttyACM0` on Linux. Close any terminal using that port first.

```sh
python tools/dongle.py update --dongle-port PORT keyboard.uf2 --install
```

The keyboard can be unplugged. It must be in dongle mode, paired and awake when
the transfer starts. Input has priority over the update traffic. The keyboard
checks available battery voltage and refuses to start or install below 3.7 V
unless USB power is present.

Without `--install`, the command downloads the image but leaves the running
firmware unchanged. To inspect it in the receiver's terminal:

```text
keyboard update status
```

After a complete download, `keyboard update commit` verifies it and records the
installation request. `keyboard update reboot` then restarts the keyboard.
Do not repeat a commit or reboot automatically if its reply is lost; check status
first. If the keyboard has not restarted, add `--resume` to continue a matching
download from its acknowledged position. After a restart, begin again without
`--resume`. Neither option overwrites a committed or unconfirmed trial image.

## What happens on the keyboard

The incoming image and fallback each have 452 KiB available. The incoming image
uses three separate external-flash regions; their addresses are defined in
[`apex_update_layout.h`](apex_update_layout.h) and listed in the
[flash map](../docs/FLASH_MEMORY_MAP.md). No settings relocation is needed.

The receiver sends ordered binary chunks through the authenticated radio stream. The
keyboard reads each write back, then checks the full image's SHA-256, board
marker, size and reset vectors before requesting installation. This checks
integrity; it is not a firmware-signing scheme.

During a transfer, the utility uses the receiver's serial port in binary mode.
USB blocks carry a CRC and receive a reply only after the keyboard has accepted
their contents. The keyboard polls the stream every millisecond while blocks
arrive, using slots with no keyboard or media input queued. Analog state remains
eligible every 10 ms when there is no queued keyboard input. Normal
10 ms diagnostic polling resumes two seconds after update traffic stops.

The earlier text-command transfer was too slow and is not used by the utility.
Binary-transfer speed and typing under that load are still being measured.

On reboot, the bootloader checks the image again before touching internal flash.
It compares and copies one page at a time, so it can continue after an interrupted
copy. The old fallback remains intact until the new application passes its normal
health checks. Repeated failed starts restore that fallback.

## Build and test

Build the bootloader with:

```sh
python tools/build_release.py --bootloader-only
```

Append `--extra-conf apex-zmk-g4b/g4b_radio_update.conf` to the
[radio keyboard build](../dongle/README.md). The option is off by default while
hardware testing is in progress.

The portable engine tests cover a full-size image, protected flash regions,
partial writes and resets during installation, malformed images and filesystem
checks. The ARM tests run the compiled bootloader with mocked flash I/O; they do
not substitute for reset testing on hardware.

```sh
python update/test_update.py --tinycrypt /path/to/zmk-upstream/modules/crypto/tinycrypt/lib
python tools/test_wireless_update.py
python update/test_bootloader.py /path/to/bootloader.elf
python update/test_bootloader_flash.py /path/to/bootloader.elf
```

The first test needs a native C compiler. The ARM tests need `unicorn` and
`pyelftools` installed in Python. `test_bootloader_flash.py` runs the real
Adafruit page-cache code against a flash model that enforces the 1→0 program
rule and counts writes per word between erases, so it exercises the install copy
the mocked `test_bootloader.py` cannot.
