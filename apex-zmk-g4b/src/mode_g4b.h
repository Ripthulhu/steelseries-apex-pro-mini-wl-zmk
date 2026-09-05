/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_MODE_H
#define APEX_G4B_MODE_H

#include <stdbool.h>
#include <stdint.h>

/* The three-position slide switch, measured on P0.03 (AIN1).
 *
 * Bluetooth 0 V, USB-only ~1.6 V, dongle ~3.3 V. Identified by sweeping the
 * five non-link analog pins while the switch was cycled: P0.03 swung the full
 * rail (0..3365 mV) and every other candidate stayed flat within 82 mV.
 */
enum g4b_mode {
    G4B_MODE_BT = 0,
    G4B_MODE_USB = 1,
    G4B_MODE_DONGLE = 2,
};

/* Take one reading of the switch and update everything below.
 *
 * Called ONLY from the watchdog thread (wdt_g4b.c), once a second, immediately
 * before that thread's feed decision. It lives there rather than on a work
 * queue because the feed depends on it: anything that can delay the sample can
 * delay the feed, and the watchdog is 60 s. The accessors below are read from
 * other threads without a lock - they are aligned scalars, updated in place,
 * and every consumer wants "the most recent reading" rather than a consistent
 * set, which is the same rule s3_fill_live() already follows.
 */
void g4b_mode_sample(void);

enum g4b_mode g4b_mode_get(void);
uint16_t g4b_mode_mv(void);

/* Software override of the physical switch. @mode is a g4b_mode to force, or an
 * out-of-range value (e.g. -1) to follow the switch again. Read via g4b_mode_get()
 * so every consumer honours it. Kept in NOINIT RAM: survives the warm reset that
 * reassigns radio ownership, but not a power cycle. */
void g4b_mode_set_override(int mode);

/* Current override, or -1 when following the switch. */
int g4b_mode_get_override(void);

/* True if the switch was in the dongle position at boot. Latched once in
 * g4b_mode_init(), so it names the RADIO owner chosen for this launch (BLE
 * controller vs. the ESB stack) - see radio_g4b.c. */
bool g4b_mode_boot_dongle(void);

/* Sticky observations about the switch line, for record_s3.mode_class.
 *
 * DIAGNOSTIC ONLY - nothing gates on these and the feed decision at
 * wdt_g4b.c:158 is untouched. They exist because a single live millivolt
 * reading cannot disambiguate: a pin sitting at
 * 3300 mV looks identical whether the switch is genuinely in the dongle
 * position or the divider is shorted on this unit. If the operator moves the
 * switch during a run and BOTH bits end up set, the line demonstrably swings
 * and the classification is reporting a real position. If only SEEN_HIGH is
 * ever set, the line never left the top rail and the hardware is the question.
 *
 * PRESENT is stamped by the writer, not by this module, so that a record from a
 * build without this field (a zero byte) is distinguishable from one where the
 * sampler genuinely never ran.
 */
#define G4B_MODE_CLASS_MASK  0x0Fu
#define G4B_MODE_SEEN_HIGH   0x10u /* a sample read >= the dongle threshold */
#define G4B_MODE_SEEN_LOW    0x20u /* a sample read <  the dongle threshold */
#define G4B_MODE_CLASS_PRESENT 0x80u

uint8_t g4b_mode_flags(void);

/* Uptime of the most recent successful switch read.
 *
 * This is the liveness proof the watchdog feeder requires. The escape hatch is
 * only real while something is actually reading the switch, so the feeder
 * refuses to feed if this goes stale - a dead mode module then reaches
 * recovery by itself instead of feeding forever with no way back.
 */
uint32_t g4b_mode_last_sample_ms(void);

#endif /* APEX_G4B_MODE_H */
