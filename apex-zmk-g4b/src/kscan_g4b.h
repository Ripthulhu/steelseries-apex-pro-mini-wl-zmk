/* SPDX-License-Identifier: MIT
 *
 * G4B kscan constants.
 *
 * Values match apex-zmk-slot/include/zmk_keyboard_apex_pro_mini_wl/protocol.h.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

enum {
    APEX_G4B_FRAME_SIZE = 64,
    APEX_G4B_EVENT_SIZE = 19,
    APEX_G4B_KEY_COUNT = 70,
    APEX_G4B_KEY_BITMAP_SIZE = 9,
};

/* The A1 response carries the key bitmap at rx[0..8] with status at rx[9] and
 * no opcode echo. Confirmed against com5-key-semantics-a-s-20260802: bit 29
 * toggles cleanly with the 'A' key across press and release.
 */
#define APEX_G4B_A1_BITMAP_OFFSET 0
#define APEX_G4B_A1_STATUS_OFFSET 9

int apex_g4b_kscan_ingest_bitmap(const struct device *dev,
                                 const uint8_t *bitmap, size_t bitmap_size);

/* APEX_G4B_KEY_BITMAP_SIZE bytes; a 1 bit marks a scan position that maps to a
 * real key. Stock masks the same way, from a table built once at boot.
 */
const uint8_t *apex_g4b_kscan_valid_mask(void);
