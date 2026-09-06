/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_RGB_OVERLAY_H
#define APEX_G4B_RGB_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Fn-layer key indicator + mode-toggle flash overlay.
 *
 * While the Fn layer (layer 1) is held, the array is replaced with a per-key
 * legend: mode toggles show green (on) / red (off), and the other Fn functions
 * light by category. Toggling a mode also flashes that key green twice (enabled)
 * or red twice (disabled), even when Fn is not held.
 *
 * The overlay is composited into the transmit frame in g4b_rgb_show(), so the
 * staged base effect is never disturbed; when the overlay clears, the base
 * repaints on the next flush. All calls except g4b_rgb_overlay_flash() run on
 * the g4b thread; the flash entry point is safe from a behavior thread.
 */

/* Composite the overlay into a built 198-channel transmit frame (LED order
 * B,G,R after the 2-byte page header). No-op when nothing is active. */
void g4b_rgb_overlay_apply(uint8_t *out_frame, uint32_t now_ms);

/* True while the Fn layer is held or a flash is animating (or was, last tick),
 * so the caller keeps flushing and a release repaints the base once. */
bool g4b_rgb_overlay_tick(uint32_t now_ms);

/* Read-only: would the overlay modify the frame right now? g4b_rgb_show() uses
 * this to decide whether it can take the untouched-frame fast path. */
bool g4b_rgb_overlay_wants(uint32_t now_ms);

/* Flash the key carrying base HID `hid` green twice (on=true) or red twice
 * (on=false). Called from the toggle behaviors; coalesces onto a small pool. */
void g4b_rgb_overlay_flash(uint8_t hid, bool on);

#endif /* APEX_G4B_RGB_OVERLAY_H */
