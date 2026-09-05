/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_ACTUATION_H
#define APEX_G4B_ACTUATION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Runtime actuation and rapid-trigger controls for the keymap (Fn layer: I/O =
 * actuation deeper/shallower, T = rapid trigger).
 *
 * Called from ZMK's thread: each moves a byte and raises a flag; the g4b thread
 * sends the frames (SPIM3 has one writer). Nothing here touches the bus.
 *
 * The actuation ladder is six measured points - 1.0, 1.5, 2.0, 2.1, 2.5, 3.0 mm.
 * The scanner indexes a non-linear lookup table in its flash, so arithmetic
 * stepping would land on unverified depths. Table in link_g4b.c.
 */

/* Move the actuation point one step deeper (+1) or shallower (-1). Clamped at
 * both ends of the ladder, and a no-op there rather than a pointless resend.
 */
void g4b_actuation_step(int delta);

/* Per-key actuation overrides, keyed by STM32 scan index (0..APEX_G4B_KEY_COUNT-1).
 * g4b_act_key_set takes a depth in tenths of a mm (2..38, clamped) and re-pushes the
 * thresholds; tenths 0 clears the override. g4b_act_key_tenths returns the key's
 * effective depth (override or global). */
int      g4b_act_key_set(uint32_t key, uint8_t tenths);
uint8_t  g4b_act_key_tenths(uint32_t key);
bool     g4b_act_key_is_override(uint32_t key);
uint32_t g4b_act_key_override_count(void);

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

/* --- diagnostics for the apex shell (implemented in link_g4b.c) ------------ */

/* STM32 scanner-link health counters. */
struct g4b_link_stats {
    uint32_t frames_run;      /* boot handshake: scanner frames issued */
    uint32_t frames_matched;  /* boot handshake: frames whose reply validated */
    uint32_t ingest_calls;    /* boot probe: bitmaps handed to ZMK's kscan */
    uint32_t ingest_ok;       /* boot probe: those it accepted */
    /* Live counters from the permanent scan loop (s3_run_keyboard); the four above
     * are one-shot boot snapshots that stop moving once the keyboard loop starts. */
    uint32_t live_key_events; /* accepted key state changes */
    uint32_t live_keepalives; /* idle 0xA0 keep-alive polls between events */
};
void g4b_link_stats_get(struct g4b_link_stats *out);

/* Filtered per-key Hall travel, raw ADC (~337 released .. ~3959 pressed;
 * 0 = not sampled yet). Only fresh while analog sampling runs; g4b_depth_force()
 * makes the link thread sample 0xA2 even without an analog RGB effect selected.
 * RGB builds only. Returns the true slot count regardless of @max. */
size_t g4b_depth_read(uint16_t *out, size_t max);
void   g4b_depth_force(bool on);

/* Shell-requested component resets/power, serviced on the g4b thread at its
 * single-writer-safe point. The caller (shell) only raises the request. */
void g4b_request_stm32_reset(void);   /* pulse STM32 EN low/high (scanner reboot) */
void g4b_request_rgb_reset(void);     /* RGB rail cycle + IS31 re-init */
void g4b_request_rgb_rail(bool on);   /* RGB rail power up(+init)/down */
void g4b_request_usb_rail_reset(void);/* pulse USB rail P0.25 (re-enumerate; always restores) */

/* Debug: send one raw 64-byte frame to the STM32 scanner and read its 64-byte
 * reply (into a caller buffer of at least 64 bytes). Runs the exchange on the
 * g4b thread at the single-writer-safe point. Returns 0 on a clean exchange.
 * Callers MUST NOT send the damaging opcodes 0x01/0x02/0x32. */
int g4b_scan_raw(const uint8_t *tx, uint32_t len, uint8_t *rx, uint32_t timeout_ms);

#endif /* APEX_G4B_ACTUATION_H */
