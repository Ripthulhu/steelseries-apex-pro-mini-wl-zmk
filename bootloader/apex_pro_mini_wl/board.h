/* SPDX-License-Identifier: MIT
 *
 * Adafruit_nRF52_Bootloader board definition for the SteelSeries Apex Pro Mini
 * Wireless (nRF52833). Derived from bluemicro_nrf52833.
 *
 * P1.04 is the only unused GPIO available to the inherited status-LED code.
 * The visible status indicator is implemented through the IS31FL3743B matrix.
 * P0.07 is the STM32-link MISO and must not be configured as a NeoPixel output.
 * P0.25 enables the USB data path and must remain under board power control.
 * P0.18 is nRESET and provides double-reset DFU entry.
 */

#ifndef _APEX_PRO_MINI_WL_H
#define _APEX_PRO_MINI_WL_H

/*------------------------------------------------------------------*/
/* LED
 *------------------------------------------------------------------*/
#define LEDS_NUMBER          1
#define LED_PRIMARY_PIN      PINNUM(1, 4)
#define LED_SECONDARY_PIN    PINNUM(1, 4) /* unused when LEDS_NUMBER is one */
#define LED_STATE_ON         1

/* No addressable RGB for the bootloader: P0.07 is SPIM MISO. */
#define NEOPIXELS_NUMBER     0

/*------------------------------------------------------------------*/
/* BUTTON  (DFU = double-tap RESET on P0.18)
 *------------------------------------------------------------------*/
#define BUTTON_DFU     PINNUM(0, 18) /* nRESET - double-tap enters DFU */
#define BUTTON_DFU_OTA PINNUM(1, 1)  /* unused on this board */
#define BUTTON_PULL    NRF_GPIO_PIN_PULLUP

//--------------------------------------------------------------------+
// BLE OTA
//--------------------------------------------------------------------+
#define BLEDIS_MANUFACTURER "SteelSeries"
#define BLEDIS_MODEL        "Apex Pro Mini WL (nRF52833)"

//--------------------------------------------------------------------+
// USB descriptors (bootloader/DFU mode only; the app enumerates as 1d50:615e)
//--------------------------------------------------------------------+
#define USB_DESC_VID          0x1d50
#define USB_DESC_UF2_PID      0x616f
#define USB_DESC_CDC_ONLY_PID 0x616f

#define UF2_PRODUCT_NAME      "Apex Pro Mini WL"
#define UF2_VOLUME_LABEL      "APEXBOOT"
#define UF2_BOARD_ID          "nRF52833-ApexProMiniWL-v1"
#define UF2_INDEX_URL         "https://steelseries.com/"

//--------------------------------------------------------------------+
// Apex-specific hardening / UX
//--------------------------------------------------------------------+
/* Application UF2 writes start above the MBR. Bootloader self-update uses a
 * separate UF2 family and is not subject to this limit. */
#define APEX_APP_FLASH_START  0x1000UL

/* pinconfig.c uses the IS31FL3743B matrix for the visible DFU indicator. */
#define BOARD_HAS_RGB_DFU     1

#endif // _APEX_PRO_MINI_WL_H
