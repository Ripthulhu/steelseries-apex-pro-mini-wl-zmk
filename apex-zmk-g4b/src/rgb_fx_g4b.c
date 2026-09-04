/* SPDX-License-Identifier: MIT
 *
 * High-rate RGB effects for the IS31FL3743B. A 200-byte PWM transfer takes
 * about 400 us at 4 Mbit/s. Rendering near 200 Hz leaves most bus time free and
 * supports temporal dithering of the controller's 8-bit channels. The renderer
 * uses fixed-point arithmetic and performs no dynamic allocation.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_underglow.h>

#include "rgb_fx_g4b.h"
#include "rgb_g4b.h"
#include "rgb_map_g4b.h"

/* Q4 fixed point for the dither accumulator: value<<4, so the low nibble is the
 * fraction that gets carried between frames.
 */
#define G4B_FX_FRAC_BITS 4u
#define G4B_FX_ONE       (1u << G4B_FX_FRAC_BITS)

/* Analog and heat-map effects fade to black over about five seconds. */
#define G4B_FX_FADE_FRAMES  1000u

static enum g4b_fx fx_current;
static uint32_t fx_last_ms;
static uint16_t fx_phase;

/* Per-channel dither residue and effect-specific per-LED state. Effects clear
 * the shared state when selected. */
static uint8_t fx_resid[G4B_RGB_CHANNELS];
static uint8_t fx_heat[G4B_RGB_LEDS];
static uint8_t fx_field_next[G4B_RGB_LEDS];

/* Reactive excitation, per LED. A key event slams the LED's entry to full and
 * the reactive render decays it; the analog render overwrites it each frame
 * with live depth. volatile because g4b_fx_key/g4b_fx_depth run on the same
 * g4b thread as the render but the writes are single bytes and order does not
 * matter - last writer wins, which is the intent.
 */
static volatile uint8_t fx_excite[G4B_RGB_LEDS];

/* Live per-key depth from 0xA2, written by g4b_fx_depth and read by the analog
 * effects. Separate from fx_excite so bitmap-driven effects never manufacture
 * a Hall reading.
 */
static volatile uint8_t fx_live[G4B_RGB_LEDS];

/* Four overlapping event-driven rings are enough to preserve rapid chords
 * without an O(keys^2) history. Radius is 16-bit because the approximate
 * corner-to-corner distance is greater than 255 in our 0..255 geometry.
 */
#define G4B_FX_WAVES 4u
struct fx_wave {
    uint8_t x;
    uint8_t y;
    uint8_t energy;
    uint8_t hue;
    uint16_t radius;
};
static struct fx_wave fx_waves[G4B_FX_WAVES];
static uint8_t fx_wave_next;

/* Digital rain uses Q4 vertical positions for sub-pixel motion. Ten drops are
 * dense enough to cover this 60% board without a large particle allocation.
 */
#define G4B_FX_RAIN_DROPS 10u
struct fx_raindrop {
    uint8_t x;
    uint8_t speed_q4;
    uint8_t hue;
    uint16_t y_q4;
};
static struct fx_raindrop fx_rain[G4B_FX_RAIN_DROPS];
static bool fx_rain_ready;

/* Heat-map effect only: per-key auto-calibration. The raw 0xA2 depth only ever
 * covers part of 0..255 and each key idles at a different value, so a fixed
 * mapping sat at green and never reached red. Instead learn each key's resting
 * depth (running min) and its deepest press (running max), and map between
 * them - so the hardest you press a key reads red and lighter reads green,
 * whatever the real range turns out to be. lo primes high / hi primes low so
 * the first real samples pull them to the truth.
 *
 * seen guards the baseline against the memset(fx_live, 0) done on entry: until
 * a key has had a real 0xA2 reading its live value is a placeholder 0, and if
 * lo snapped to that 0 it would stay there (min never rises on its own), so the
 * key's genuine resting depth would later read as a large press and glow. So a
 * key is dark and uncalibrated until g4b_fx_depth marks it seen.
 */
static uint8_t fx_hm_lo[G4B_RGB_LEDS];
static uint8_t fx_hm_hi[G4B_RGB_LEDS];
static uint8_t fx_hm_seen[G4B_RGB_LEDS];

/* Shared decaying display level for the analog and heat-map effects, held in Q8
 * (value<<8) so the small per-frame decrement of a slow fade still has room
 * below the 8-bit output. fx_dec is the per-key decrement, recomputed on each
 * attack as peak/FADE_FRAMES so the fade always lasts ~5 s regardless of the
 * peak height. Each key attacks instantly to the live press intensity and then
 * decays, so a key lit by a press fades out over ~5 s after release instead of
 * snapping back the instant the finger lifts.
 */
static uint16_t fx_level[G4B_RGB_LEDS];
static uint16_t fx_dec[G4B_RGB_LEDS];

/* Attack-and-decay one key: rise instantly to `inst` (0..255) and re-arm the
 * fade, otherwise fall by the peak-sized decrement. Returns 0..255.
 */
static uint8_t fx_decay(uint8_t led, uint8_t inst)
{
    uint16_t lvl = fx_level[led];
    uint16_t inst_q8 = (uint16_t)((uint16_t)inst << 8);

    if (inst_q8 >= lvl) {
        /* New peak: jump to it and size the decrement so THIS peak takes
         * FADE_FRAMES to reach zero. +1 guards against a zero step. */
        lvl = inst_q8;
        fx_dec[led] = (uint16_t)(lvl / G4B_FX_FADE_FRAMES) + 1u;
    } else {
        uint16_t dec = fx_dec[led] ? fx_dec[led] : 1u;

        lvl = (lvl > dec) ? (uint16_t)(lvl - dec) : 0u;
    }
    fx_level[led] = lvl;
    return (uint8_t)(lvl >> 8);
}

void g4b_fx_key(uint8_t scan_idx)
{
    uint8_t led;

    if (scan_idx >= G4B_SCAN_SLOTS) {
        return;
    }
    led = g4b_scan_led[scan_idx];
    if (led >= G4B_RGB_LEDS) {
        return;
    }

    if (fx_current == G4B_FX_REACTIVE) {
        fx_excite[led] = 255u;
    } else if (fx_current == G4B_FX_SHOCKWAVE) {
        struct fx_wave *wave = &fx_waves[fx_wave_next];

        wave->x = g4b_led_x[led];
        wave->y = g4b_led_y[led];
        wave->energy = 255u;
        wave->hue = (uint8_t)(fx_phase + g4b_led_x[led]);
        wave->radius = 0u;
        fx_wave_next = (uint8_t)((fx_wave_next + 1u) % G4B_FX_WAVES);
    } else if (fx_current == G4B_FX_INK) {
        fx_heat[led] = 255u;
    }
}

void g4b_fx_depth(uint8_t scan_idx, uint8_t q8)
{
    uint8_t led;

    if (scan_idx >= G4B_SCAN_SLOTS) {
        return;
    }
    led = g4b_scan_led[scan_idx];
    if (led < G4B_RGB_LEDS) {
        fx_live[led] = q8;
        fx_hm_seen[led] = 1u; /* this key now has a real reading (heat map) */
    }
}

bool g4b_fx_wants_analog(void)
{
    return fx_current == G4B_FX_ANALOG || fx_current == G4B_FX_HEATMAP;
}

/* xorshift, not rand(). Deterministic, three instructions, and no libc. */
static uint32_t fx_rng_state = 0x1234567u;

static uint32_t fx_rand(void)
{
    uint32_t x = fx_rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fx_rng_state = x;
    return x;
}

/* Sine as a quarter-wave table plus reflection: 64 bytes instead of 256, and
 * the reflection is two compares. Returns 0..255 centred on 128.
 */
static const uint8_t fx_sin_q[64] = {
    128, 131, 134, 137, 140, 143, 146, 149, 152, 156, 159, 162, 165, 167, 170,
    173, 176, 179, 182, 184, 187, 190, 192, 195, 197, 200, 202, 204, 207, 209,
    211, 213, 215, 217, 219, 221, 223, 224, 226, 228, 229, 231, 232, 233, 235,
    236, 237, 238, 239, 240, 241, 242, 243, 243, 244, 245, 245, 246, 246, 246,
    247, 247, 247, 247,
};

static uint8_t fx_sin(uint8_t angle)
{
    if (angle < 64u) {
        return fx_sin_q[angle];
    }
    if (angle < 128u) {
        return fx_sin_q[127u - angle];
    }
    if (angle < 192u) {
        return (uint8_t)(255u - fx_sin_q[angle - 128u]);
    }
    return (uint8_t)(255u - fx_sin_q[255u - angle]);
}

/* Hue to RGB, 8-bit, no saturation or value parameters - the effects below set
 * their own brightness, so a full-value ramp is all that is wanted here.
 */
static void fx_hue(uint8_t h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t seg = (uint8_t)(h / 43u);
    uint8_t off = (uint8_t)((h - seg * 43u) * 6u);

    switch (seg) {
    case 0: *r = 255u; *g = off; *b = 0u; break;
    case 1: *r = (uint8_t)(255u - off); *g = 255u; *b = 0u; break;
    case 2: *r = 0u; *g = 255u; *b = off; break;
    case 3: *r = 0u; *g = (uint8_t)(255u - off); *b = 255u; break;
    case 4: *r = off; *g = 0u; *b = 255u; break;
    default: *r = 255u; *g = 0u; *b = (uint8_t)(255u - off); break;
    }
}

/* Write one channel through the dither accumulator.
 *
 * `value` is Q4. The integer part goes out; the fraction accumulates until it
 * carries, at which point one extra PWM code goes out for one frame. Averaged
 * over a handful of frames the channel sits between two codes, which is the
 * whole point.
 */
static uint8_t fx_dither(uint32_t idx, uint16_t value_q4)
{
    uint32_t whole = value_q4 >> G4B_FX_FRAC_BITS;
    uint32_t frac = value_q4 & (G4B_FX_ONE - 1u);
    uint32_t acc = (uint32_t)fx_resid[idx] + frac;

    if (acc >= G4B_FX_ONE) {
        acc -= G4B_FX_ONE;
        whole++;
    }
    fx_resid[idx] = (uint8_t)acc;

    return (uint8_t)((whole > 255u) ? 255u : whole);
}

/* Global brightness, 0..255, refreshed each frame from ZMK's underglow state so
 * the Fn brightness keys (RGB_BRI/RGB_BRD) drive our effects too. Without this
 * the effects rendered at a fixed level and the brightness control did nothing -
 * which also made them look dim, because a low persisted brightness could not be
 * turned back up. CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX remains the hard output cap,
 * exactly as it is for ZMK's own effects.
 */
static uint8_t fx_bright = 255u;

static void fx_refresh_brightness(void)
{
    /* calc_brt(0) returns the current colour with brightness unchanged; .b is
     * the HSV value in 0..100. Note that is NOT CONFIG_..._BRT_MAX: ZMK clamps
     * the stored value against a hardcoded BRT_MAX of 100 (rgb_underglow.c) and
     * only applies the config cap when converting to PWM. So .b really can reach
     * 100. Apply the configured output ceiling separately, matching ZMK's
     * hsb_scale_zero_max(); dividing by the config max would instead overflow
     * this uint8_t once .b climbed above it. A read of ZMK's static state,
     * unlocked - a single byte, benign to race.
     */
    uint32_t b = zmk_rgb_underglow_calc_brt(0).b;

    if (b > 100u) {
        b = 100u;
    }
    fx_bright = (uint8_t)((b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * 255u) /
                          (100u * 100u));
}

static void fx_set_dithered(uint8_t led, uint16_t r_q4, uint16_t g_q4,
                            uint16_t b_q4)
{
    uint32_t base = (uint32_t)led * 3u;

    /* Scale by the global brightness. Applied here so every effect obeys it
     * without each render having to, and before the dither so the fractional
     * bits still carry - a brightness cut deepens the dither rather than
     * coarsening it.
     */
    r_q4 = (uint16_t)(((uint32_t)r_q4 * fx_bright) / 255u);
    g_q4 = (uint16_t)(((uint32_t)g_q4 * fx_bright) / 255u);
    b_q4 = (uint16_t)(((uint32_t)b_q4 * fx_bright) / 255u);

    g4b_rgb_set_pixel(led, fx_dither(base + 0u, r_q4),
                      fx_dither(base + 1u, g_q4),
                      fx_dither(base + 2u, b_q4));
}

/* Plasma in PHYSICAL space, not index space.
 *
 * Now that g4b_led_x / g4b_led_y give each LED a real board coordinate, the
 * interfering sines run over position rather than over LED number, so the
 * pattern moves diagonally across the actual keyboard and reads as flowing
 * blobs instead of the bands the index-space version produced. This is what the
 * LED map bought.
 */
static void fx_render_plasma(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint8_t a = fx_sin((uint8_t)((g4b_led_x[i] >> 3) + (fx_phase >> 2)));
        uint8_t b = fx_sin((uint8_t)((g4b_led_y[i] >> 3) - (fx_phase >> 3)));
        uint8_t h = (uint8_t)((a + b) >> 1);
        uint8_t rr, gg, bb;

        fx_hue(h, &rr, &gg, &bb);
        fx_set_dithered(i, (uint16_t)(rr * 12u), (uint16_t)(gg * 12u),
                        (uint16_t)(bb * 12u));
    }
}

/* A ring that expands from the centre of the board, using real geometry: each
 * LED lights as the wavefront's radius passes its distance from centre. With
 * the physical map this is a clean circle; without it, it was not expressible
 * at all.
 */
static void fx_render_ripple(void)
{
    uint8_t cx = 128u, cy = 128u;
    uint8_t front = (uint8_t)(fx_phase << 1);

    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        int16_t dx = (int16_t)g4b_led_x[i] - (int16_t)cx;
        int16_t dy = (int16_t)g4b_led_y[i] - (int16_t)cy;
        uint32_t d2 = (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
        /* Cheap integer sqrt by bit-by-bit; d2 fits in 17 bits so this is a
         * dozen iterations, well inside the frame budget for 66 LEDs.
         */
        uint32_t r = 0u, bit = 1u << 16;

        while (bit > d2) {
            bit >>= 2;
        }
        while (bit != 0u) {
            if (d2 >= r + bit) {
                d2 -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }

        uint8_t phase = (uint8_t)((uint8_t)r - front);
        uint8_t v = fx_sin(phase);          /* bright at the wavefront */
        uint8_t hue = (uint8_t)(160u + (r >> 2));

        if (v < 128u) {
            v = 0u;
        } else {
            v = (uint8_t)(((uint32_t)v - 128u) * 2u);
        }
        uint8_t rr, gg, bb;

        fx_hue(hue, &rr, &gg, &bb);
        fx_set_dithered(i, (uint16_t)(rr * v / 24u), (uint16_t)(gg * v / 24u),
                        (uint16_t)(bb * v / 24u));
    }
}

/* Classic cooling/spark fire, per LED. The dither matters most here: embers
 * spend their life in the bottom twenty PWM codes, exactly where 8 bits bands.
 */
static void fx_render_fire(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint32_t cool = fx_rand() % 12u;

        fx_heat[i] = (uint8_t)((fx_heat[i] > cool) ? (fx_heat[i] - cool) : 0u);
    }
    /* Diffuse along the strip so heat spreads instead of flickering per LED. */
    for (uint8_t i = (uint8_t)(G4B_RGB_LEDS - 1u); i >= 2u; i--) {
        fx_heat[i] = (uint8_t)(((uint32_t)fx_heat[i - 1u] +
                                fx_heat[i - 2u] + fx_heat[i - 2u]) / 3u);
    }
    if ((fx_rand() % 100u) < 40u) {
        uint8_t at = (uint8_t)(fx_rand() % 8u);

        fx_heat[at] = (uint8_t)(160u + (fx_rand() % 95u));
    }

    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint8_t h = fx_heat[i];
        uint16_t r = (uint16_t)(h * 16u);
        uint16_t g = (uint16_t)((h > 128u) ? ((h - 128u) * 32u) : 0u);
        uint16_t b = (uint16_t)((h > 224u) ? ((h - 224u) * 64u) : 0u);

        fx_set_dithered(i, r, g, b);
    }
}

/* Three slow sines of different periods summed - a cheap stand-in for noise
 * that never repeats visibly, drifting through blues and greens.
 */
static void fx_render_aurora(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint32_t n = (uint32_t)fx_sin((uint8_t)(i * 2u + (fx_phase >> 4))) +
                     (uint32_t)fx_sin((uint8_t)(i * 3u - (fx_phase >> 5))) +
                     (uint32_t)fx_sin((uint8_t)(i + (fx_phase >> 6)));
        uint8_t v = (uint8_t)(n / 3u);
        /* Hue confined to 100..160, the green-through-blue arc. */
        uint8_t h = (uint8_t)(100u + (v / 4u));
        uint8_t rr, gg, bb;

        fx_hue(h, &rr, &gg, &bb);
        fx_set_dithered(i, (uint16_t)(rr * v / 24u), (uint16_t)(gg * v / 24u),
                        (uint16_t)(bb * v / 24u));
    }
}

/* Dim base wash with bright decaying sparks. The decay is per frame, so at
 * 200 Hz a spark falls smoothly over about a second rather than in the visible
 * staircase 20 Hz would give.
 */
static void fx_render_sparkle(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        if (fx_heat[i] > 2u) {
            fx_heat[i] = (uint8_t)(fx_heat[i] - 2u);
        } else {
            fx_heat[i] = 0u;
        }
    }
    if ((fx_rand() % 100u) < 25u) {
        fx_heat[fx_rand() % G4B_RGB_LEDS] = 255u;
    }

    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint16_t base = 10u * G4B_FX_ONE / 4u;
        uint16_t s = (uint16_t)(fx_heat[i] * 14u);

        fx_set_dithered(i, (uint16_t)(base + s), (uint16_t)(base + s * 3u / 4u),
                        (uint16_t)(base * 3u + s));
    }
}

/* Keys flare white-hot on press and fade through their own hue, using the map
 * so the flare lands on the right LED. Driven by the key bitmap we already
 * ingest - no extra scanner traffic at all.
 */
static void fx_render_reactive(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint8_t e = fx_excite[i];
        uint16_t r, g, b;

        if (e > 4u) {
            fx_excite[i] = (uint8_t)(e - 4u); /* ~1 s fall at 200 Hz */
        } else {
            fx_excite[i] = 0u;
        }

        /* Hot core fading to a colour keyed by board position, so a burst of
         * typing paints a moving gradient rather than a uniform white.
         */
        uint8_t hue = (uint8_t)(g4b_led_x[i]);
        uint8_t rr, gg, bb;

        fx_hue(hue, &rr, &gg, &bb);
        /* Blend white (near full excite) toward the hue (low excite). */
        r = (uint16_t)((e * 16u + rr * (16u - (e >> 4))) >> 0);
        g = (uint16_t)((e * 16u + gg * (16u - (e >> 4))) >> 0);
        b = (uint16_t)((e * 16u + bb * (16u - (e >> 4))) >> 0);
        r = (uint16_t)(r * e / 255u);
        g = (uint16_t)(g * e / 255u);
        b = (uint16_t)(b * e / 255u);
        fx_set_dithered(i, r, g, b);
    }
}

/* Brightness follows how far each key is physically pressed. The excite buffer
 * here is live depth from 0xA2, written by g4b_fx_depth, not a decaying flare.
 * A resting key sits near zero and glows dim; press it and it blooms. This is
 * the effect the per-key Hall sensors make possible and almost no keyboard can
 * do.
 */
static void fx_render_analog(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        /* Decay the live depth so a press fades pink -> purple -> blue over ~5 s
         * rather than dropping back to blue the instant the key is released. */
        uint8_t d = fx_decay(i, fx_live[i]);
        /* A dim floor so an untouched board is still lit, then depth on top. */
        uint16_t base = 6u * G4B_FX_ONE / 4u;
        uint16_t lift = (uint16_t)(d * 14u);
        /* Blue at rest, red when deep, via magenta - NOT through green and
         * yellow. 170 + up to 85 keeps the whole ramp on the cool-to-hot side
         * of the wheel, so a hard press is actually red. */
        uint8_t hue = (uint8_t)(170u + ((uint32_t)d * 85u) / 255u);
        uint8_t rr, gg, bb;

        fx_hue(hue, &rr, &gg, &bb);
        fx_set_dithered(i, (uint16_t)(base + (uint32_t)rr * lift / 255u),
                        (uint16_t)(base + (uint32_t)gg * lift / 255u),
                        (uint16_t)(base + (uint32_t)bb * lift / 255u));
    }
}

/* Fast Euclidean approximation: max + half min. It is circular enough on a
 * key grid, costs no multiply/sqrt, and is stable all the way to opposite
 * corners of the 0..255 physical map.
 */
static uint16_t fx_distance(uint8_t ax, uint8_t ay, uint8_t bx, uint8_t by)
{
    uint16_t dx = (ax > bx) ? (uint16_t)(ax - bx) : (uint16_t)(bx - ax);
    uint16_t dy = (ay > by) ? (uint16_t)(ay - by) : (uint16_t)(by - ay);
    uint16_t hi = MAX(dx, dy);
    uint16_t lo = MIN(dx, dy);

    return (uint16_t)(hi + (lo >> 1));
}

/* Every key-down launches a ring at that key's real physical position. Up to
 * four rings overlap by taking the brightest contribution in each channel,
 * which keeps a chord colourful without clipping the entire board to white.
 * Shockwave is bitmap-driven and adds no 0xA2 scanner traffic.
 */
static void fx_render_shockwave(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint16_t out_r = 0u, out_g = 0u, out_b = 0u;

        for (uint8_t w = 0u; w < G4B_FX_WAVES; w++) {
            const struct fx_wave *wave = &fx_waves[w];
            uint16_t d, gap;
            uint8_t v, rr, gg, bb;
            uint16_t r, g, b;

            if (wave->energy == 0u) {
                continue;
            }
            d = fx_distance(g4b_led_x[i], g4b_led_y[i], wave->x, wave->y);
            gap = (d > wave->radius) ? (d - wave->radius) : (wave->radius - d);
            if (gap >= 20u) {
                continue;
            }
            v = (uint8_t)(((uint32_t)wave->energy * (20u - gap)) / 20u);
            fx_hue(wave->hue, &rr, &gg, &bb);
            r = (uint16_t)(((uint32_t)rr * v * 16u) / 255u);
            g = (uint16_t)(((uint32_t)gg * v * 16u) / 255u);
            b = (uint16_t)(((uint32_t)bb * v * 16u) / 255u);
            out_r = MAX(out_r, r);
            out_g = MAX(out_g, g);
            out_b = MAX(out_b, b);
        }
        fx_set_dithered(i, out_r, out_g, out_b);
    }

    for (uint8_t w = 0u; w < G4B_FX_WAVES; w++) {
        if (fx_waves[w].energy != 0u) {
            fx_waves[w].radius = (uint16_t)(fx_waves[w].radius + 2u);
            fx_waves[w].energy--;
        }
    }
}

/* A typing heat map with no background at all. Where G4B_FX_ANALOG sits on a
 * blue floor and warms to red, this one is black until a key moves: a light
 * press lights it green, and pressing deeper walks the hue down the wheel to
 * red at full travel. Brightness ramps fast - full by roughly half travel - so
 * a light touch reads as a clear green rather than a dim glow, leaving the
 * green->red colour to carry how hard the key is pressed across the whole
 * range. Follows live depth directly (no hold/fade), so it tracks the key.
 */
static void fx_render_heatmap(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint8_t d = fx_live[i];
        uint8_t lo = fx_hm_lo[i];
        uint8_t hi = fx_hm_hi[i];
        uint16_t span, p, n, val;
        uint8_t hue, rr, gg, bb;

        if (!fx_hm_seen[i]) {
            fx_set_dithered(i, 0u, 0u, 0u); /* no real reading yet: stay dark */
            continue;
        }

        /* Track the calibration window. New minimum -> lower the rest floor;
         * new maximum -> raise the ceiling. When neither, let the ceiling decay
         * a hair every 128 frames (~0.6 s) so one unusually hard jab does not
         * raise the bar for red permanently. Because hi includes the current
         * frame, the deepest instant of any press has d == hi, i.e. n == 255,
         * i.e. red - so "as hard as you press this key" always reaches red.
         *
         * lo snaps down instantly but also leaks up ~50/s toward the live
         * value, so the resting baseline follows slow thermal drift instead of
         * sticking at an old low and letting the key creep green. A real press
         * is far too brief for the leak to catch it; a long hold cools slowly,
         * which is fitting for a heat map.
         */
        if (d < lo) {
            lo = d;
            fx_hm_lo[i] = lo;
        } else if ((fx_phase & 0x03u) == 0u && lo < d) {
            lo = (uint8_t)(lo + 1u);
            fx_hm_lo[i] = lo;
        }
        if (d > hi) {
            hi = d;
            fx_hm_hi[i] = hi;
        } else if ((fx_phase & 0x7fu) == 0u && hi > lo) {
            hi = (uint8_t)(hi - 1u);
            fx_hm_hi[i] = hi;
        }

        /* A small deadzone above rest keeps sensor noise and slow thermal drift
         * dark, which is what makes "no background" hold. Until a key has been
         * pressed far enough to calibrate (hi barely above lo), use a full-scale
         * span so an early touch reads as a dim green rather than flashing red.
         */
        p = (d > (uint16_t)lo + 6u) ? (uint16_t)(d - lo - 6u) : 0u;
        span = (hi > (uint16_t)lo + 20u) ? (uint16_t)(hi - lo) : 255u;
        n = (uint16_t)(((uint32_t)p * 255u) / span);
        if (n > 255u) {
            n = 255u;
        }
        /* Attack to the live press intensity, then decay: a hard press blooms
         * red at once and cools back through green to black over ~5 s, instead
         * of tracking the finger instantly.
         */
        n = fx_decay(i, (uint8_t)n);
        if (n == 0u) {
            fx_set_dithered(i, 0u, 0u, 0u); /* rested and fully faded: dark */
            continue;
        }

        /* Green (85) at the lightest press down to red (0) at the hardest, with
         * brightness floored so even a light press is a clearly visible green
         * and a full press is bright red.
         */
        hue = (uint8_t)(85u - ((uint32_t)n * 85u) / 255u);
        val = (uint16_t)(64u + ((uint32_t)n * 191u) / 255u); /* 64..255 */
        fx_hue(hue, &rr, &gg, &bb);
        fx_set_dithered(i, (uint16_t)(((uint32_t)rr * val * 16u) / 255u),
                        (uint16_t)(((uint32_t)gg * val * 16u) / 255u),
                        (uint16_t)(((uint32_t)bb * val * 16u) / 255u));
    }
}

static void fx_rain_seed_drop(struct fx_raindrop *drop, bool anywhere)
{
    drop->x = (uint8_t)fx_rand();
    drop->speed_q4 = (uint8_t)(12u + (fx_rand() % 20u));
    drop->hue = (uint8_t)(96u + (fx_rand() % 65u));
    drop->y_q4 = anywhere ? (uint16_t)((fx_rand() % 352u) << 4) :
                            (uint16_t)((fx_rand() % 32u) << 4);
}

/* Ten independently moving heads cast long, narrow trails down the physical
 * keyboard. Positions are fractional, colours stay in the green/cyan/blue
 * arc, and overlapping drops combine by channel rather than erasing each
 * other. It is entirely procedural: NOR space is not executable code space.
 */
static void fx_render_rain(void)
{
    if (!fx_rain_ready) {
        for (uint8_t d = 0u; d < G4B_FX_RAIN_DROPS; d++) {
            fx_rain_seed_drop(&fx_rain[d], true);
        }
        fx_rain_ready = true;
    }

    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint16_t out_r = 0u, out_g = 0u, out_b = 0u;

        for (uint8_t d = 0u; d < G4B_FX_RAIN_DROPS; d++) {
            const struct fx_raindrop *drop = &fx_rain[d];
            uint16_t head = drop->y_q4 >> 4;
            uint16_t dy, dx, v;
            uint8_t rr, gg, bb;
            uint16_t r, g, b;

            if (head < g4b_led_y[i]) {
                continue;
            }
            dy = head - g4b_led_y[i];
            dx = (drop->x > g4b_led_x[i]) ?
                 (uint16_t)(drop->x - g4b_led_x[i]) :
                 (uint16_t)(g4b_led_x[i] - drop->x);
            if (dy > 72u || dx > 18u) {
                continue;
            }

            v = (uint16_t)(255u - (dy * 3u));
            v = (uint16_t)((v * (18u - dx)) / 18u);
            fx_hue(drop->hue, &rr, &gg, &bb);
            r = (uint16_t)(((uint32_t)rr * v * 16u) / 255u);
            g = (uint16_t)(((uint32_t)gg * v * 16u) / 255u);
            b = (uint16_t)(((uint32_t)bb * v * 16u) / 255u);
            out_r = MAX(out_r, r);
            out_g = MAX(out_g, g);
            out_b = MAX(out_b, b);
        }
        fx_set_dithered(i, out_r, out_g, out_b);
    }

    for (uint8_t d = 0u; d < G4B_FX_RAIN_DROPS; d++) {
        fx_rain[d].y_q4 = (uint16_t)(fx_rain[d].y_q4 + fx_rain[d].speed_q4);
        if ((fx_rain[d].y_q4 >> 4) > 336u) {
            fx_rain_seed_drop(&fx_rain[d], false);
        }
    }
}

static uint8_t fx_ink_neighbor(uint8_t row, uint8_t col)
{
    uint8_t scan = (uint8_t)(row * 14u + col);
    uint8_t led = g4b_scan_led[scan];

    return (led < G4B_RGB_LEDS) ? fx_heat[led] : 0u;
}

static uint8_t fx_ink_merge(uint8_t best, uint8_t neighbor)
{
    uint8_t spread = (neighbor > 10u) ? (uint8_t)(neighbor - 10u) : 0u;

    return MAX(best, spread);
}

/* Key-down injects energy into the 5x14 scan grid. Each frame retains a slowly
 * fading core and borrows attenuated energy from its four neighbours, making
 * colour clouds spread across nearby keys. The colour itself follows physical
 * X and time, so overlapping clouds stay visibly fluid rather than becoming a
 * single flat flare.
 */
static void fx_render_ink(void)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint8_t e = fx_heat[i];
        uint8_t hue = (uint8_t)(g4b_led_x[i] + (fx_phase >> 1));
        uint8_t rr, gg, bb;

        fx_hue(hue, &rr, &gg, &bb);
        fx_set_dithered(i, (uint16_t)(((uint32_t)rr * e * 16u) / 255u),
                        (uint16_t)(((uint32_t)gg * e * 16u) / 255u),
                        (uint16_t)(((uint32_t)bb * e * 16u) / 255u));
    }

    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        uint8_t scan = g4b_led_scan[i];
        uint8_t row = (uint8_t)(scan / 14u);
        uint8_t col = (uint8_t)(scan % 14u);
        uint8_t best = (fx_heat[i] > 2u) ? (uint8_t)(fx_heat[i] - 2u) : 0u;

        if (col != 0u) {
            best = fx_ink_merge(best,
                                fx_ink_neighbor(row, (uint8_t)(col - 1u)));
        }
        if (col < 13u) {
            best = fx_ink_merge(best,
                                fx_ink_neighbor(row, (uint8_t)(col + 1u)));
        }
        if (row != 0u) {
            best = fx_ink_merge(best,
                                fx_ink_neighbor((uint8_t)(row - 1u), col));
        }
        if (row < 4u) {
            best = fx_ink_merge(best,
                                fx_ink_neighbor((uint8_t)(row + 1u), col));
        }
        fx_field_next[i] = best;
    }
    memcpy(fx_heat, fx_field_next, sizeof(fx_heat));
}

static void fx_reset_state(void)
{
    memset(fx_resid, 0, sizeof(fx_resid));
    memset(fx_heat, 0, sizeof(fx_heat));
    memset(fx_field_next, 0, sizeof(fx_field_next));
    memset((void *)fx_excite, 0, sizeof(fx_excite));
    memset((void *)fx_live, 0, sizeof(fx_live));
    memset(fx_waves, 0, sizeof(fx_waves));
    memset(fx_rain, 0, sizeof(fx_rain));
    fx_wave_next = 0u;
    fx_rain_ready = false;
    /* Prime heat-map calibration above/below the valid sample range. */
    memset(fx_hm_lo, 0xFF, sizeof(fx_hm_lo));
    memset(fx_hm_hi, 0, sizeof(fx_hm_hi));
    memset(fx_hm_seen, 0, sizeof(fx_hm_seen));
    memset(fx_level, 0, sizeof(fx_level));
    memset(fx_dec, 0, sizeof(fx_dec));
}

void g4b_fx_cycle(void)
{
    fx_current = (enum g4b_fx)((fx_current + 1) % G4B_FX_COUNT);

    /* Leaving an effect must not leave its residue behind: the dither carries
     * state between frames, and ZMK's next frame would inherit a stale
     * fractional offset on every channel.
     */
    fx_reset_state();
}

enum g4b_fx g4b_fx_current(void)
{
    return fx_current;
}

void g4b_fx_set(enum g4b_fx fx)
{
    if (fx < G4B_FX_COUNT) {
        fx_current = fx;
        /* Studio can jump directly between effects, so use exactly the same
         * state reset as the keyboard cycle action. */
        fx_reset_state();
    }
}

bool g4b_fx_tick(void)
{
    uint32_t now;

    if (fx_current == G4B_FX_ZMK) {
        return false;
    }

    now = k_uptime_get_32();
    if ((now - fx_last_ms) < G4B_FX_PERIOD_MS) {
        /* Not due, but still ours: returning true keeps ZMK's staged frame from
         * being transmitted underneath us between our frames.
         */
        return true;
    }
    fx_last_ms = now;
    fx_phase++;
    fx_refresh_brightness();

    switch (fx_current) {
    case G4B_FX_PLASMA:  fx_render_plasma(); break;
    case G4B_FX_FIRE:    fx_render_fire(); break;
    case G4B_FX_AURORA:  fx_render_aurora(); break;
    case G4B_FX_SPARKLE: fx_render_sparkle(); break;
    case G4B_FX_RIPPLE:  fx_render_ripple(); break;
    case G4B_FX_REACTIVE: fx_render_reactive(); break;
    case G4B_FX_ANALOG:   fx_render_analog(); break;
    case G4B_FX_SHOCKWAVE: fx_render_shockwave(); break;
    case G4B_FX_HEATMAP:  fx_render_heatmap(); break;
    case G4B_FX_RAIN:     fx_render_rain(); break;
    case G4B_FX_INK:      fx_render_ink(); break;
    default: break;
    }

    g4b_rgb_mark_pending();
    return true;
}
