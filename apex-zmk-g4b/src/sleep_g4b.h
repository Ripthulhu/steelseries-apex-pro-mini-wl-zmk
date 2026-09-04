/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_SLEEP_H
#define APEX_G4B_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

/* Optional System OFF implementation for the STM32-backed scanner. ZMK deep
 * sleep cannot configure this board's non-matrix wake sources. Entry requires:
 *
 * - Bluetooth mode, where P0.03 is at a valid digital-low level.
 * - P0.24 attention low, so the sense condition is not already active.
 * - A real attention event observed in GPIO LATCH during the current boot.
 *
 * System OFF remains disabled in the supported configuration because wake
 * requires a reset and Bluetooth reconnection.
 */

/* Arm SENSE High on ATTN (P0.24) and start watching LATCH.
 *
 * Called once, after keyboard_link_reassert() has finished configuring pins,
 * because that writes PIN_CNF wholesale and would clear the SENSE field.
 *
 * The mode pin is deliberately NOT armed here. It is a SAADC input, and
 * connecting its digital input buffer while the payload is running risks
 * disturbing the very readings the watchdog feeder gates on. It is armed in
 * g4b_sleep_enter(), microseconds before the chip stops executing.
 */
void g4b_sleep_arm(void);

/* Sample and clear GPIO LATCH from the scan loop. */
void g4b_sleep_poll_latch(void);

/* Return true after a key event has set the P0.24 LATCH bit this boot. */
bool g4b_sleep_wake_proven(void);

/* Raw GPIO LATCH, for the diagnostic line emitted next to the sleep attempt. */
uint32_t g4b_sleep_latch_raw(void);

/* Configure the mode pin as the second wake source and stop the CPU.
 *
 * Normally does not return - the next instruction this core executes is a reset
 * vector. It DOES return if the SYSTEMOFF write had no effect, so the caller
 * can record that and go back to being a keyboard rather than parking forever.
 */
void g4b_sleep_enter(void);

#endif /* APEX_G4B_SLEEP_H */
