/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_RGB_MAP_H
#define APEX_G4B_RGB_MAP_H

#include <stdint.h>

#include "rgb_g4b.h"

/* LED-to-HID mapping recovered from the 66-byte table at stock nRF file offset
 * 0x28248 and cross-checked against the SteelSeries device specification.
 * g4b_led_x/g4b_led_y use 0..255 physical keyboard coordinates for spatial
 * effects rather than the controller's electrical 6x11 channel order.
 */

#define G4B_SCAN_SLOTS 70u

extern const uint8_t g4b_led_hid[G4B_RGB_LEDS];
extern const uint8_t g4b_led_x[G4B_RGB_LEDS];
extern const uint8_t g4b_led_y[G4B_RGB_LEDS];

/* g4b_led_scan[led] = scan index (0..69) of the key under that LED. Its inverse
 * g4b_scan_led[scan] = the LED for that scan slot, or 0xFF. These join the
 * lighting map to the key and analog paths: a key event or a 0xA2 depth sample
 * arrives by scan index, and these say which LED it drives.
 */
extern const uint8_t g4b_led_scan[G4B_RGB_LEDS];
extern const uint8_t g4b_scan_led[G4B_SCAN_SLOTS];

/* The LED under a given HID usage, or 0xFF if none. Built once at init from the
 * forward table, so a key-reactive effect can light the key that was pressed.
 */
uint8_t g4b_led_for_hid(uint8_t hid);

#endif /* APEX_G4B_RGB_MAP_H */
