/* SPDX-License-Identifier: MIT */
#ifndef APEX_GAMEPAD_H
#define APEX_GAMEPAD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APEX_GAMEPAD_REPORT_SIZE 11
#define APEX_GAMEPAD_PAYLOAD_SIZE 13
#define APEX_GAMEPAD_VERSION 1

/* X, Y, Z, Rz, Rx: unsigned 0..32767, followed by eight buttons. */
#define APEX_GAMEPAD_DESCRIPTOR \
    0x05,0x01,0x09,0x05,0xa1,0x01,0xa1,0x00, \
    0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,0x09,0x33, \
    0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x05,0x81,0x02,0xc0, \
    0x05,0x09,0x19,0x01,0x29,0x08,0x15,0x00,0x25,0x01, \
    0x75,0x01,0x95,0x08,0x81,0x02,0xc0

static inline void apex_gamepad_neutral(uint8_t report[APEX_GAMEPAD_REPORT_SIZE])
{
    memset(report, 0, APEX_GAMEPAD_REPORT_SIZE);
    report[1] = report[3] = report[9] = 0x40;
}

static inline bool apex_gamepad_valid(const uint8_t *payload, size_t length)
{
    if (length != APEX_GAMEPAD_PAYLOAD_SIZE || payload[0] != APEX_GAMEPAD_VERSION ||
        payload[1] > 1) return false;
    for (unsigned int i = 3; i < 12; i += 2) {
        if (payload[i] & 0x80) return false;
    }
    return true;
}
/* Call only after CCM authentication, and reset last_counter for a new session. */
static inline bool apex_gamepad_newer(uint32_t counter, uint32_t *last_counter)
{
    if (counter <= *last_counter) return false;
    *last_counter = counter;
    return true;
}
#endif
