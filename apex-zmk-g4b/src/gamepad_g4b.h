/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_GAMEPAD_H
#define APEX_G4B_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

/* Axis range as declared in the report descriptor: 0..32767, centre 16384.
 * Z and Rz are one-sided and rest at 0.
 */
#define G4B_GP_MAX    32767
#define G4B_GP_CENTRE 16384

/* Stage the report and ask for it to be sent. Safe to call from the g4b thread
 * as often as samples arrive: the actual USB write happens on a work item, and
 * re-submitting while one is pending is a no-op, so this is self-rate-limiting
 * and never blocks the caller.
 */
void g4b_gamepad_publish(uint16_t x, uint16_t y, uint16_t z, uint16_t rz,
                         uint16_t rx,
                         uint8_t buttons);

/* The gamepad is off at boot and toggled from the keymap. Toggling it rebuilds
 * the USB configuration: enabling adds the gamepad HID interface, and disabling
 * removes it. Some games automatically select the first controller they see.
 */
void g4b_gamepad_set_enabled(bool on);
bool g4b_gamepad_is_enabled(void);

/* Counters, so a dead axis can be told from a busy endpoint in the record. */
extern uint32_t g4b_gp_writes;
extern uint32_t g4b_gp_busy;
extern uint32_t g4b_gp_err;

#endif /* APEX_G4B_GAMEPAD_H */
