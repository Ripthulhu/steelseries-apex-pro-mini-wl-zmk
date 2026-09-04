/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_RGB_H
#define APEX_G4B_RGB_H

#include <stdbool.h>
#include <stdint.h>

/* IS31FL3743B on SPIM2. 66 RGB LEDs = 198 channels.
 *
 * Bus, read off the hardware and the stock driver:
 *   SCK  P1.09, MOSI P1.08 (SPIM2 PSEL, from the loader-left snapshot)
 *   CS   P0.11 GPIO, idle high (stock init writes PIN_CNF[11]=3 at 0x296C8)
 *   4 Mbit/s, mode 0, MSB first
 *   rail enables P0.19 + P0.23, already raised by g4b_vendor_rails_init
 *
 * Wire format, from the stock write path at 0x293C0 / 0x29414:
 *   CS low, { command, 1-based register, data... }, CS high
 *     command 0x50 = PWM page      (registers 1..198)
 *     command 0x51 = scaling page  (registers 1..198)
 *     command 0x52 = function page (config / global current / pull)
 */

#define G4B_RGB_LEDS     66u
#define G4B_RGB_CHANNELS (G4B_RGB_LEDS * 3u)

/* Configure the bus and bring the controller out of shutdown with a full
 * scaling page, so PWM writes actually light. Returns 0 on success.
 */
int g4b_rgb_init(void);

/* Full cold bring-up of the controller, callable repeatedly. Needed on every
 * rise of the P0.19 rail, because the part loses all 396 registers while it is
 * down. g4b_rgb_init() calls this after configuring the bus.
 */
void g4b_rgb_bringup(void);

/* Set one LED in the local frame buffer. Nothing lights until g4b_rgb_show(). */
void g4b_rgb_set_pixel(uint8_t led, uint8_t r, uint8_t g, uint8_t b);

/* Raw register write, no colour interpretation - for the mapping probe. */
void g4b_rgb_set_raw(uint8_t led, uint8_t reg0, uint8_t reg1, uint8_t reg2);

/* Fill the whole frame buffer with one colour. */
void g4b_rgb_set_all(uint8_t r, uint8_t g, uint8_t b);

/* Push the frame buffer to the PWM page in one transfer. */
void g4b_rgb_show(void);

/* Deferred transmit, so that every SPIM2 transfer happens on one thread and the
 * payload needs no bus lock.
 *   g4b_rgb_mark_pending() - flag the buffer dirty (called from ZMK's underglow
 *     workqueue; does not touch the bus).
 *   g4b_rgb_flush() - if dirty, transmit the frame. Call only from
 *     the g4b thread; see the invariant note in rgb_g4b.c.
 */
void g4b_rgb_mark_pending(void);
void g4b_rgb_flush(void);

/* Blank the array without telling ZMK anything about it.
 *
 * Entering blank pushes one all-zero PWM page, parks SPIM2/CS, and lowers the
 * controller rails. g4b_rgb_flush() becomes a no-op that still clears the
 * pending flag, so underglow can keep ticking into the frame buffer and simply
 * not reach the LEDs. Leaving blank restores the bus before rail/controller
 * initialization and re-arms the pending flag, so the next flush repaints.
 *
 * The staged buffer is never overwritten, which is the point: this is an output
 * gate, not a state change. Underglow does not learn it was switched off, so
 * nothing is persisted and there is nothing for the user to switch back on.
 * Call from the g4b thread, like every other SPIM2 access.
 */
void g4b_rgb_set_blanked(bool blank);

/* Backlight idle fade. g4b_rgb_idle_tick() eases the whole array toward on
 * (want_on) or off over ~10 s and cuts/raises the rail at the ends; drive it
 * once per keyboard-loop pass with the current uptime. g4b_rgb_fading() reports
 * whether a ramp is still in flight, so the loop can keep ticking fast enough
 * to render the fade smoothly instead of throttling to the idle poll rate.
 * Call both functions from the g4b thread, like every other SPIM2 access.
 */
void g4b_rgb_idle_tick(bool want_on, uint32_t now);
bool g4b_rgb_fading(void);
bool g4b_rgb_is_blanked(void);

#endif /* APEX_G4B_RGB_H */
