# Third-party notices

Project-authored code is licensed under the MIT License in [`LICENSE`](LICENSE).
Files carrying another SPDX identifier remain under that license.

The firmware is built with third-party projects whose source distributions
contain their complete notices, including:

- ZMK, MIT License
- Zephyr RTOS, Apache License 2.0 and component-specific licenses
- Nordic Semiconductor nrfx and hardware abstraction code, BSD-3-Clause and
  component-specific Nordic licenses
- Adafruit nRF52 Bootloader, MIT License
- Microsoft UF2 reference implementation, MIT License
- TinyUSB, MIT License
- TinyCrypt, BSD-3-Clause License
- nanopb, zlib License
- littlefs, BSD-3-Clause License
- Mbed TLS, Apache License 2.0
- picolibc/newlib, BSD-family component licenses
- zcbor, Apache License 2.0

Release ZIPs include the corresponding license texts. They also include the
license supplied with the Nordic MBR image and the S140 7.2.0 license shipped
with the bootloader source.

`apex-zmk-g4b/src/link_g4b.c` includes the Nordic USB errata helper sequence
adapted from Zephyr's `nrf_usbd_common.c`. That portion is copyright Nordic
Semiconductor ASA and licensed under Apache-2.0; the rest of that file is MIT.

The current checkout and release downloads do not contain SteelSeries firmware,
per-device calibration data, or private device backups.
Protocol constants, packet layouts, and wiring information were independently
observed for interoperability with hardware owned by the project contributors.
SteelSeries is a trademark of its owner; this project is not affiliated with or
endorsed by SteelSeries.
