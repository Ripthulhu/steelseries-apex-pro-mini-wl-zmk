/* SPDX-License-Identifier: MIT
 *
 * Adafruit_nRF52_Bootloader board definition for the SteelSeries Apex Pro Mini
 * Wireless 2.4 GHz USB dongle (nRF52833, aQFN73, N52833 QIAA). Derived from the
 * keyboard's apex_pro_mini_wl board, stripped for the dongle:
 *
 *   - The dongle's D+/D- go straight to the nRF52833 USB pins. There is NO
 *     external USB data-path mux (the keyboard's P0.25/U10), so board_init2()
 *     is empty and USB enumerates with no board bring-up.
 *   - No IS31FL3743B RGB matrix -> BOARD_HAS_RGB_DFU is undefined; the status
 *     indicator is a single inherited PWM LED.
 *   - No external NOR / A/B rollback (that is gated on the keyboard board in
 *     CMakeLists.txt, so nothing here needs to opt out).
 *
 * Installed flash map (same as the keyboard, from linker/nrf52833.ld):
 *   0x00000  Nordic MBR (from bootloader_mbr.hex)
 *   0x01000  application slot (first UF2 lands here)
 *   0x74000  this bootloader
 *   0x7E000  MBR parameter page ; 0x7F000 bootloader settings
 *   UICR 0x10001014 = 0x74000 (boot start) ; 0x10001018 = 0x7E000 (mbr params)
 *
 * The dongle runs its GPIO rail at 1.8 V (REGOUT0). USB uses its own VBUS-fed
 * 3.3 V regulator and is independent of REGOUT0, so USB works at 1.8 V VDD.
 * UICR_REGOUT0_VALUE is intentionally NOT defined here: the factory UICR value
 * must be preserved by the installer, not rewritten by the bootloader.
 */

#ifndef _APEX_DONGLE_WL_H
#define _APEX_DONGLE_WL_H

/*------------------------------------------------------------------*/
/* LED
 *------------------------------------------------------------------*/
/* PROVISIONAL: the dongle's single status LED pin is not yet confirmed from a
 * hardware probe. P0.07 is driven as an output by the stock dongle firmware and
 * is a safe generic GPIO (not NFC/reset/XL). Driving the wrong pin here is only
 * cosmetic (no USB or radio pin is a GPIO on the nRF52833). Verify on hardware
 * and correct if the dongle LED does not blink in DFU. */
#define LEDS_NUMBER          1
#define LED_PRIMARY_PIN      PINNUM(0, 7)
#define LED_SECONDARY_PIN    PINNUM(0, 7) /* unused when LEDS_NUMBER is one */
#define LED_STATE_ON         1

/* No addressable RGB on the dongle. */
#define NEOPIXELS_NUMBER     0

/*------------------------------------------------------------------*/
/* BUTTON  (DFU entry)
 *------------------------------------------------------------------*/
/* The dongle has no user button. Double-tap nRESET (P0.18) still works if the
 * reset line can be pulsed; the primary DFU paths are (a) auto-DFU when the app
 * slot is empty/invalid and (b) the app writing GPREGRET=0x57 then resetting. */
#define BUTTON_DFU     PINNUM(0, 18) /* nRESET */
#define BUTTON_DFU_OTA PINNUM(1, 1)  /* unused on this board */
#define BUTTON_PULL    NRF_GPIO_PIN_PULLUP

//--------------------------------------------------------------------+
// BLE OTA
//--------------------------------------------------------------------+
#define BLEDIS_MANUFACTURER "SteelSeries"
#define BLEDIS_MODEL        "Apex Pro Mini WL Dongle (nRF52833)"

//--------------------------------------------------------------------+
// USB descriptors (bootloader/DFU mode only)
//--------------------------------------------------------------------+
/* Distinct PID from the keyboard bootloader (0x616f) so the host shows the
 * dongle's APEXBOOT as its own device. */
#define USB_DESC_VID          0x1d50
#define USB_DESC_UF2_PID      0x6170
#define USB_DESC_CDC_ONLY_PID 0x6170

#define UF2_PRODUCT_NAME      "Apex Pro Mini WL Dongle"
#define UF2_VOLUME_LABEL      "APEXDONGLE"
#define UF2_BOARD_ID          "nRF52833-ApexProMiniWLDongle-v1"
#define UF2_INDEX_URL         "https://steelseries.com/"

//--------------------------------------------------------------------+
// Apex-specific hardening / UX
//--------------------------------------------------------------------+
/* Application UF2 writes start above the MBR. Bootloader self-update uses a
 * separate UF2 family and is not subject to this limit. */
#define APEX_APP_FLASH_START  0x1000UL

#endif // _APEX_DONGLE_WL_H
