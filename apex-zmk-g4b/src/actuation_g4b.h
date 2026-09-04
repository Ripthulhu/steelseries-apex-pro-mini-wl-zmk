/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_ACTUATION_H
#define APEX_G4B_ACTUATION_H

#include <stdint.h>

/* Runtime actuation and rapid-trigger controls, for the keymap.
 *
 * The keycaps have always carried these legends - I and O for actuation up and
 * down, T for rapid trigger - and they were inert because the settings were
 * build-time constants. They are runtime state now.
 *
 * All of these are called from ZMK's thread. They move a byte and raise a flag;
 * the g4b thread notices and sends the frames, because SPIM3 has exactly one
 * writer. Nothing here touches the bus.
 *
 * The actuation ladder is six MEASURED points - 1.0, 1.5, 2.0, 2.1, 2.5 and
 * 3.0 mm - because the scanner takes indices into a lookup table in its own
 * flash and the mapping is not linear. Stepping arithmetically would be
 * inventing depths nobody has verified. See link_g4b.c for the table.
 */

/* Move the actuation point one step deeper (+1) or shallower (-1). Clamped at
 * both ends of the ladder, and a no-op there rather than a pointless resend.
 */
void g4b_actuation_step(int delta);

/* Adjust rapid trigger sensitivity in tenths of a millimetre, clamped to the
 * scanner's 1..40. It will not step down into 0 - switching the feature off is
 * the toggle's job, so that leaning on the decrement key cannot disable it.
 */
void g4b_rapid_trigger_step(int delta);

/* Cycle the reset point through its four gaps below the press point: 2, 4, 6,
 * 8 counts, wrapping. Expressed as a gap so press stays above release at every
 * actuation setting - a key resting exactly on the threshold chatters.
 */
void g4b_reset_point_cycle(void);

uint8_t g4b_reset_gap_counts(void);

/* Off, or back to the last non-zero sensitivity. */
void g4b_rapid_trigger_toggle(void);

/* Current settings, in tenths of a millimetre. Rapid trigger reads 0 when off.
 * For a display, or a future way of reporting state to the host.
 */
/* Ask for the switch settings to be written to NVS, debounced by 30 s.
 *
 * Called by anything that changes them. Debounced because each of these is a
 * key someone presses repeatedly while dialling in a feel, and a flash write
 * per press is both NVS wear and a radio-synced erase that blocks its caller.
 */
void g4b_settings_mark_dirty(void);

uint8_t g4b_actuation_tenths(void);
uint8_t g4b_rapid_trigger_tenths(void);

#endif /* APEX_G4B_ACTUATION_H */
