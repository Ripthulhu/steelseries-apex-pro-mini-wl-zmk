# Reverse engineering the Apex Pro Mini Wireless

This is how we worked out the first-generation keyboard and its USB dongle.
The project replaces the Nordic firmware on both devices. The keyboard's STM32
still runs the stock scanner firmware, including its factory calibration.

If you want to install it, start with [the installation guide](../INSTALL.md).
This section explains the hardware and the experiments behind the port.

## Start with the two processors

The Nordic isn't measuring the switches itself. It asks an STM32G0 for key
states over SPI, then handles the keymap, USB and wireless connections. This
split let us replace the keyboard firmware without replacing the Hall scanner.

The other devices are fairly conventional once their connections are known:
an IS31FL3743B RGB driver, a BQ25895 charger and a 1 MiB SPI NOR flash. The
awkward part was identifying the pins, startup order and scanner handshake.
The [hardware reference](../HARDWARE.md) has the photographs, pinout and bus map.

The dongle is simpler. It's another nRF52833 with direct USB, a crystal and an
antenna. It has no button, indicator LED or external flash. It also doesn't
have the keyboard's USB data-path switch. Reusing a keyboard board definition
for it would drive pins that have no established purpose on the receiver.

## Finding the firmware

The useful firmware was in the GG installer, not a public keyboard source repo.
GG includes separate Nordic, STM32 and receiver update files, plus device
descriptions that tell its updater how to send them.

The files weren't encrypted firmware containers. They were application images
with vectors, code and version strings we could analyse directly. That didn't
make them complete device backups. In particular, the keyboard update files
didn't contain its protected factory bootloader or per-key scanner calibration.

We followed GG's device definitions as well as its executable code. Searching
for an "enter bootloader" string alone found a headset update path, which
wasn't how this keyboard updated. The actual keyboard definitions used a
file-transfer API against both its normal and recovery USB identities.

[Stock USB updates and bootloaders](STOCK_USB.md) covers that investigation,
the image addresses, and the different installation routes for the two devices.
The [SteelSeries repository review](../STEELSERIES_SOURCES.md) records what the
public source repositories did and didn't provide.

## Getting USB to appear

A running Nordic application didn't necessarily appear on USB. P0.25 controls
U10, which connects the USB data pair to the Nordic. Charging can work while
that connection is off, so a powered keyboard isn't proof that USB reaches it.

The stock image configures P0.25 as an output, raises it during USB bring-up,
and lowers it on a transport transition. Reproducing that GPIO setup was needed
in the application and the replacement bootloader.

Later we separated USB power from USB input selection. VBUS means a cable
provides power. A usable USB data connection means there's a host to send keys
to. Treating those as the same condition would break Bluetooth when connected
to a battery bank.

[USB data path](USB_DATA_PATH.md) has the register writes and the current policy.
The exact U10 part number remains unidentified. Its observed switching behaviour
is enough for this port, but it isn't a complete identification of the chip.

## Getting keys out of the STM32

The scanner needed more than an SPI clock and an opcode. Each request and reply
is a separate 64-byte transfer, with a READY handshake between them. There is
no conventional chip-select. A second line, ATTN, signals a queued key event.

Capturing the stock startup sequence gave us 59 configuration exchanges.
Replaying them got the scanner into the expected state. Disassembling the STM32
then explained the tables, actuation values and commands behind that replay.
The captured bytes remain a regression check, not a substitute for understanding
which commands can write calibration.

The key report is an absolute bitmap. Pressing known keys tied individual bits
to physical positions, including Fn and the irregular bottom row. The scanner
already decides whether a key is pressed. Adding a second matching-report
requirement on the Nordic would delay input and could wait forever after ATTN
went low.

[Scanner investigation](SCANNER.md) explains the disassembly, calibration,
neighbour table and sleep experiments. [The protocol reference](../PROTOCOL.md)
contains the frame formats and command details.

## Lighting and a misleading read-back result

The RGB driver has 198 channels, enough for 66 RGB positions. This keyboard has
61 keys, so not every position is populated. The stock writes revealed its page
commands and startup values. The port sends a whole PWM page instead of issuing
one transaction per LED.

We initially treated the driver as write-only because the normal code left MISO
disconnected. That was a software choice, not a board limitation. Connecting
P0.08 and reading back a written `0x5A` sentinel proved the return path worked.
Reading zero fault bits alone wouldn't have proved anything, because a floating
line could also read zero.

The open-channel test then explained an actual fault on the development board.
It found five unpopulated RGB triplets and two open channels on the `2` key.
That matched the key lighting red but not blue or green. It wasn't an effect
or colour-mapping bug.

[RGB controller](RGB_CONTROLLER.md) records the test, register map and channel
mapping. The faulty LED is a finding about this unit, not an expected property
of the keyboard model.

## Storage and power

The external NOR gave us somewhere to keep settings, Bluetooth bonds, a recovery
image and crash records. It isn't extra executable memory. Recovery copies an
image back into internal flash before running it.

Removing the old SoftDevice reservation moved the ZMK application from the
early `0x27000` layout to `0x1000`. Zephyr supplies the Bluetooth controller, so
keeping that reservation was wasting both flash and RAM. The
[flash map](../FLASH_MEMORY_MAP.md) is the current layout. The proposed second
full-size wireless-update image isn't installed yet.

There are separate power problems to solve. The scanner can run less often,
the Nordic can sleep between events, and the RGB rails can be shut off entirely.
Turning PWM values to zero doesn't do all three. The idle policy now also
applies to the custom radio builds. A shorter deep-sleep test confirmed wake by
a held key and by moving the mode switch. That checks wake behaviour, not battery
life or current consumption.

The clock wiring matters here too. P0.00 and P0.01 are used by the NOR, so the
port uses the internal low-frequency RC clock. The separate 32 MHz crystal is
used for radio operation. A lack of a low-frequency crystal doesn't mean radio
timing must use a millisecond software timer.

The BQ25895 has a power path, so the custom firmware can run the keyboard from
USB without a battery. The development unit did so, while stock firmware refused
to turn on without its pack. The pack is rated for 4.4 V charging. Our lower
4.096 V ceiling and charge/passthrough policy are deliberate. A voltage-based
percentage is still an estimate, not a measurement of remaining capacity.
See [power and features](../FEATURES.md) and [configuration](../CONFIGURATION.md)
for the defaults and controls.

## The stock radio investigation

The first radio attempts mixed up several different configurations present in
the vendor images. Finding an ESB-like routine didn't establish that it was
the active pairing path. Receiving nearby Bluetooth advertisements proved that
our receiver could receive those advertisements, not that our proprietary
transmissions matched the dongle.

Later tests reached GG's "pairing successful" message. The keyboard still
flashed its pairing indicator and no keys arrived. That wasn't a working link.
Captures also disproved the early claim that the protocol was unencrypted:
recorded CCM inputs reproduced the recorded ciphertext in an independent test.

The remaining problem was the live encrypted handoff, including counter state,
framing and timing. An offline encryption match didn't solve that state machine.
We eventually chose to control both ends instead of continuing to imitate GG.

[Radio investigation](RADIO.md) keeps the useful stock findings separate from
the custom protocol. It also explains how the custom link went from individual
reports and slow acknowledgements to queued delivery and hardware-timed radio
events. The current throughput results are close to 1,000 reports/s, but aren't
a measured 1 ms key-to-application latency claim.

## Reading the evidence

This account was assembled from the saved Claude and Codex investigations,
capture notes, firmware analysis and the source now in this repository. Old
session summaries often contain a conclusion followed much later by its
correction. The correction belongs in the explanation, rather than leaving
both claims for the reader to reconcile.

Addresses in the scanner chapters refer to the STM32 **3.24.1** application at
`0x08007000`. Nordic and host executable addresses belong to their own images.
Don't use an address from one image as an offset into another.

For reproduction, start with the image identities in [Stock USB](STOCK_USB.md),
the scanner constructor and the shared radio tests. Private device backups,
pairing keys and raw chat transcripts aren't distributed. A claim based on a
local hardware capture is described as such. It isn't presented as a test that
every reader can rerun without that hardware.

The remaining unknowns include the exact STM32 suffix, the second scanner
bitmap, U10's part number, and the stock radio's first successful encrypted
handoff. None is a reason to pretend the working custom implementation is
compatible with every board revision or with the stock receiver firmware.
