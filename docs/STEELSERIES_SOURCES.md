# SteelSeries source repositories

The public repositories reviewed in the
[SteelSeries GitHub organization](https://github.com/SteelSeries) do not contain
Apex keyboard firmware, an nRF52 bootloader, STM32G0 scanner code, or a factory
firmware image.

## Embedded repositories

The three repositories with microcontroller code are unmodified upstream forks
for other STM32 families:

| Repository | Contents | Relevance |
|---|---|---|
| [`stm32f103xx-hal`](https://github.com/SteelSeries/stm32f103xx-hal) | Early Rust HAL for STM32F103 | Wrong STM32 family; no SteelSeries changes |
| [`stm32f30x-hal`](https://github.com/SteelSeries/stm32f30x-hal) | Early Rust HAL for STM32F30x | Wrong STM32 family; no product code |
| [`picobit`](https://github.com/SteelSeries/picobit) | Scheme VM with STM32F100 and F4 ports | Generic examples only |

Their public branches and tags contain no nRF52833 board support, Nordic
SoftDevice source, STM32G070 code, product DFU implementation, or keyboard
firmware.

## GameSense

[`gamesense-sdk`](https://github.com/SteelSeries/gamesense-sdk) documents the
host-facing lighting model. Its useful parts are:

- a 22×6 virtual keyboard bitmap;
- named key and zone groups;
- background layers with event-controlled regions; and
- key selection by USB HID usage code.

This could be used for an optional GameSense-to-keyboard lighting bridge. It
does not describe the keyboard's USB update protocol or either processor's
firmware. The repository has no top-level license, so its examples should not be
copied without a separate license review.

## Search scope

The check covered every public default branch, public branch and tag, repository
history, and published release asset available through the organization. Private
or deleted repositories and unreleased vendor code were not accessible.

None of the public SteelSeries repositories or updater files reviewed contains
the factory bootloader needed for a complete stock restore. It cannot be copied
from a protected keyboard over SWD; see
[Restoring stock](BOOTLOADER.md#restoring-stock).
