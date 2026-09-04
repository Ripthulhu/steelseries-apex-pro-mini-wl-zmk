/* SPDX-License-Identifier: MIT
 *
 * Generated from the stock nRF image, cross-checked against the decrypted GG
 * device spec. See rgb_map_g4b.h. Two plaintext tables compose here:
 *   0x28248  LED index  -> HID usage
 *   0x2792C  scan index -> HID usage  (5x14 row-major, matches our kscan and
 *            the 0xA2 analog ordering: scan 16/29/30/31 = W/A/S/D, verified on
 *            hardware this project)
 * so g4b_led_scan[led] = the scan index of the key under that LED, and
 * g4b_scan_led[scan] is the inverse for driving a specific LED from a key event.
 */

#include "rgb_map_g4b.h"

const uint8_t g4b_led_hid[G4B_RGB_LEDS] = {
    0x29u, 0x2Bu, 0x39u, 0xE1u, 0xE0u, 0x2Du, 0x1Eu, 0x14u, 0x04u, 0x64u, 0xE3u,
    0x2Eu, 0x1Fu, 0x1Au, 0x16u, 0x1Du, 0xE2u, 0x89u, 0x20u, 0x08u, 0x07u, 0x1Bu,
    0x2Cu, 0x2Au, 0x21u, 0x15u, 0x09u, 0x06u, 0x8Au, 0x2Fu, 0x22u, 0x17u, 0x0Au,
    0x19u, 0x88u, 0x30u, 0x23u, 0x1Cu, 0x0Bu, 0x05u, 0xE6u, 0x31u, 0x24u, 0x18u,
    0x0Du, 0x11u, 0xE7u, 0x34u, 0x25u, 0x0Cu, 0x0Eu, 0x10u, 0xF0u, 0x32u, 0x26u,
    0x12u, 0x0Fu, 0x36u, 0xE4u, 0x28u, 0x27u, 0x13u, 0x33u, 0x37u, 0x38u, 0xE5u,
};

const uint8_t g4b_led_x[G4B_RGB_LEDS] = {
    0u, 0u, 0u, 0u, 0u, 208u, 19u, 28u, 33u, 28u, 24u,
    227u, 38u, 47u, 52u, 42u, 47u, 231u, 57u, 66u, 71u, 61u,
    123u, 246u, 76u, 85u, 90u, 80u, 71u, 217u, 94u, 104u, 109u,
    99u, 71u, 236u, 113u, 123u, 128u, 118u, 198u, 255u, 132u, 142u,
    146u, 137u, 222u, 222u, 151u, 161u, 165u, 156u, 175u, 241u, 170u,
    179u, 184u, 175u, 246u, 250u, 189u, 198u, 203u, 194u, 212u, 246u,
};

const uint8_t g4b_led_y[G4B_RGB_LEDS] = {
    0u, 64u, 128u, 191u, 255u, 0u, 0u, 64u, 128u, 191u, 255u,
    0u, 0u, 64u, 128u, 191u, 255u, 191u, 0u, 64u, 128u, 191u,
    255u, 0u, 0u, 64u, 128u, 191u, 255u, 64u, 0u, 64u, 128u,
    191u, 191u, 64u, 0u, 64u, 128u, 191u, 255u, 64u, 0u, 64u,
    128u, 191u, 255u, 128u, 0u, 64u, 128u, 191u, 255u, 128u, 0u,
    64u, 128u, 191u, 255u, 128u, 0u, 64u, 128u, 191u, 191u, 191u,
};

const uint8_t g4b_led_scan[G4B_RGB_LEDS] = {
    0u, 14u, 28u, 42u, 56u, 11u, 1u, 15u, 29u, 43u, 57u,
    12u, 2u, 16u, 30u, 44u, 58u, 13u, 3u, 17u, 31u, 45u,
    60u, 67u, 4u, 18u, 32u, 46u, 61u, 25u, 5u, 19u, 33u,
    47u, 62u, 26u, 6u, 20u, 34u, 48u, 63u, 27u, 7u, 21u,
    35u, 49u, 64u, 39u, 8u, 22u, 36u, 50u, 65u, 40u, 9u,
    23u, 37u, 51u, 66u, 41u, 10u, 24u, 38u, 52u, 53u, 55u,
};

/* scan index -> LED, 0xFF where a scan slot has no LED (there are 70 scan
 * slots and 66 LEDs). */
const uint8_t g4b_scan_led[G4B_SCAN_SLOTS] = {
    0x00u, 0x06u, 0x0Cu, 0x12u, 0x18u, 0x1Eu, 0x24u, 0x2Au, 0x30u, 0x36u, 0x3Cu,
    0x05u, 0x0Bu, 0x11u, 0x01u, 0x07u, 0x0Du, 0x13u, 0x19u, 0x1Fu, 0x25u, 0x2Bu,
    0x31u, 0x37u, 0x3Du, 0x1Du, 0x23u, 0x29u, 0x02u, 0x08u, 0x0Eu, 0x14u, 0x1Au,
    0x20u, 0x26u, 0x2Cu, 0x32u, 0x38u, 0x3Eu, 0x2Fu, 0x35u, 0x3Bu, 0x03u, 0x09u,
    0x0Fu, 0x15u, 0x1Bu, 0x21u, 0x27u, 0x2Du, 0x33u, 0x39u, 0x3Fu, 0x40u, 0xFFu,
    0x41u, 0x04u, 0x0Au, 0x10u, 0xFFu, 0x16u, 0x1Cu, 0x22u, 0x28u, 0x2Eu, 0x34u,
    0x3Au, 0x17u, 0xFFu, 0xFFu,
};

uint8_t g4b_led_for_hid(uint8_t hid)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        if (g4b_led_hid[i] == hid) {
            return i;
        }
    }
    return 0xFFu;
}
