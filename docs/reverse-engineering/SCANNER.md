# How the STM32 scanner was decoded

The scanner runs independently of ZMK. It samples the Hall sensors, applies
calibration and actuation settings, then queues key states for the Nordic.
We kept that firmware because it already handles the analogue side of the board.

The [protocol reference](../PROTOCOL.md) contains the packet formats. This page
explains how they were identified and which early interpretations were wrong.

## Load the image at the right address

The vendor's STM32 3.24.1 update contains 51,200 bytes of application code.
Its actual base is `0x08007000`. An early generated ELF labelled its section
at `0x08002000`, which made addresses misleading. Use the raw image and correct
base when comparing these locations.

The command dispatcher at `0x08009198` splits the command into a direction bit
and a six-bit ID. Its 21-entry table starts at `0x0800D540`, with 16 bytes per
entry. Each entry supplies read/write function pointers, an ID and a gate byte.
Searching for a literal comparison against `0xA2` therefore misses its handler.

Following those function pointers connected commands seen on SPI to the code
that changes scanner state. It also separated ordinary RAM configuration from
the calibration-save path that writes flash.

## Capture first, then explain the startup

The stock Nordic configures the scanner in two passes before normal polling.
The captured sequence has 59 request/reply pairs. Version `3.24.1` gives the
first recognisable response, followed by per-key tables and scanner enable.

The port keeps that sequence in
[`apex_boot_prefix.h`](../../apex-zmk-g4b/apex_boot_prefix.h).
[`build_scanner_config.py`](../../apex-zmk-g4b/build_scanner_config.py)
reconstructs its transmit frames from parameters and the retained neighbour
records. You can check the reconstruction without hardware, from the repo root:

```sh
python apex-zmk-g4b/build_scanner_config.py --check
```

This checks byte agreement with the capture. It doesn't independently prove
every field's meaning. In particular, the neighbour topology is read from the
retained records rather than recreated from PCB geometry.

The build generates a compact table with one copy of each distinct 64-byte
block. Each exchange points to its original request and expected reply. This
saves flash without changing the bytes or order. The retained capture remains
unchanged, and tests expand the compact table back into all 59 exchanges.

READY and ATTN have different jobs. READY gates transfer phases, while ATTN
means a key report is waiting. Reading a key report consumes an event. A second
read isn't a debounce confirmation, and a reply needn't echo its request opcode.
Those distinctions mattered more than increasing the SPI speed.

## Actuation isn't a raw ADC threshold

The calibration loader at `0x08007FC4` validates a per-key table before using it.
Each key has two 16-bit reference values for its range. The stock actuation
bytes index the table at `0x0800D3E0`, then the scanner resolves that travel
against the individual key's calibration.

That explains why the same setting can be applied across keys with different
resting Hall readings. The conversion isn't linear. Treating a configuration
byte as an absolute ADC threshold would change the feel from key to key.

`0xA2`, handled at `0x08008D8C`, exposes the raw samples separately. Holding
known keys connected the sample positions to the key bitmap. W, A, S and D
were checked at scanner indices 16, 29, 30 and 31. The analogue gamepad uses
those samples, with its own learned rest and bottom-out ranges.

Note that controller output still needs suitable mappings in each game. Some
games need mods, and some won't handle keyboard-derived analogue input well.

## The neighbour table isn't magnetic compensation

Command `0x36` writes a nine-byte neighbour record per scanner position. The
writer stores it at `SB + 0x4D7 + key * 9`, where `SB` is the scan-state block.
The lists resemble the key grid, which initially suggested magnetic coupling.

The readers told a different story. Around `0x08007CF4` and `0x08007B40`, the
scanner sets and clears neighbour flags, back-pointers and link bytes as keys
engage and release. That's a logical key-state graph.

A Hall-sample test supported that interpretation. Holding G moved its own
sample by 2,287 counts. Every other position moved by at most four counts,
including the listed neighbours. That observation doesn't support the earlier
claim that the list compensates for substantial magnetic crosstalk.

We retain the stock records because some entries don't follow a simple
geometric rule. Replacing them with a tidy generated grid could change the
scanner's filtering. The precise purpose of every unusual neighbour remains
less certain than the fact that the records drive key-state bookkeeping.

Command `0x37` is separate. It stores a signed per-key adjustment and an enable
bit. The reader near `0x08007984` sign-extends the byte before adding it to the
actuation calculation. `0x38` stores a scalar of 500 in the captured setup.
It shouldn't be described as a measured 500 Hz scan rate.

## Why we didn't erase the scanner

The fallback calibration constants are `{928, 3136}`. Early analysis treated
the existence of this fallback as proof that an erased scanner would type
normally. That's too strong. Continuing through the scan loop doesn't prove
that those thresholds suit every sensor. A resting reading above the fallback
threshold can leave a key permanently pressed.

The SWD check found read protection: peripheral access worked, but flash reads
faulted. Removing that protection erases the chip. The vendor application file
doesn't restore the missing factory loader and per-unit calibration, so we
didn't take that route.

The save command is another way to lose calibration. `0x32` reaches flash
programming through `cal_save` at `0x08008114`. It isn't an ordinary commit of
the actuation settings. Changing actuation or rapid trigger doesn't require it.

## Slower scanning and deep sleep are different

`20 03 <period>` changes the alternate scan interval while leaving the scanner
able to detect keys. The interval is one byte, so its maximum is 255 ms.
An early request to try 1,000 ms didn't establish a one-second scanner mode.
The current idle configuration uses 50 ms, then 255 ms after a minute.

Mode 0 stops scanning. Hardware tests didn't find a reliable key or MOSI wake
from that state. It therefore isn't used as the normal idle policy.

There is also a STOP1 helper at `0x0800B380`. It sets the STM32 power-mode bits
and Cortex sleep-deep flag before sleeping. Its caller, task `0x08009672`,
controls the mailbox/RTC loop around that sleep. Finding the helper doesn't
establish a usable SPI command or an any-key wake source. We haven't demonstrated
a reliable host-controlled STOP1 cycle on this board.

The Nordic's System OFF is a different mechanism. It can wake from the scanner's
ATTN line while the scanner remains alive. The firmware checks key-wake signalling
before allowing that deeper sleep. Tests with a shortened timeout confirmed
held-key wake and mode-switch wake, including return to the custom dongle link.
Those tests don't measure the scanner's STOP1 current.

## Still unresolved

The second nine-byte bitmap in `0xA1` has the same indexing as the first, but its
meaning isn't established. Captures reviewed here had zeros there. It isn't
per-key analogue depth, which has a separate read command.

The STM32 belongs to the G0 family with 128 KiB flash. We haven't established
the exact G070/G071 suffix from the available evidence. Neither that uncertainty
nor the unused bitmap requires replacing the working scanner firmware.
