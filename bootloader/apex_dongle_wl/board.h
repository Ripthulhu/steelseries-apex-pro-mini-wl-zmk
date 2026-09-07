/* SPDX-License-Identifier: MIT */
#ifndef _APEX_DONGLE_WL_H
#define _APEX_DONGLE_WL_H

/* nRF52833 with direct USB, a crystal and no user LEDs or buttons.
 * Leave GPIOs, DC/DC configuration and the factory REGOUT0 value alone. */
#define LEDS_NUMBER 0
#define NEOPIXELS_NUMBER 0

#define BOARD_USB_DFU_ONLY
#define BOARD_USB_RECOVERY_WINDOW_MS 2000
#define BOARD_UF2_APP_ID_ONLY
#define BOARD_DFU_DEVICE_REV 0x6170

#define BLEDIS_MANUFACTURER "SteelSeries"
#define BLEDIS_MODEL "Apex Pro Mini WL Dongle (nRF52833)"

/* Existing local development IDs; not a claim of a registered allocation. */
#define USB_DESC_VID          0x1d50
#define USB_DESC_UF2_PID      0x6170
#define USB_DESC_CDC_ONLY_PID 0x6170
#define UF2_PRODUCT_NAME "Apex Pro Mini WL Dongle"
#define UF2_VOLUME_LABEL "APEXDONGLE"
#define UF2_BOARD_ID "nRF52833-ApexProMiniWLDongle-v1"
#define UF2_INDEX_URL "https://github.com/Ripthulhu/steelseries-apex-pro-mini-wl-zmk"

/* Application: [0x1000, 0x6d000). Seven reserved pages precede the
 * bootloader at 0x74000. MBR parameters/settings are at 0x7e000/0x7f000. */
#define APEX_APP_FLASH_START 0x1000UL

#endif
