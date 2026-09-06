/* SPDX-License-Identifier: MIT
 *
 * Fn-layer key indicator + mode-toggle flash overlay. See rgb_overlay_g4b.h.
 *
 * Keys are located by their base-layer HID usage (g4b_led_for_hid), not by
 * matrix position, because the bottom-row scan order is not a plain row-major
 * transform and the HID table is the authoritative key->LED map.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>
#include <zmk/battery.h>

#include "rgb_g4b.h"
#include "rgb_map_g4b.h"
#include "rgb_overlay_g4b.h"
#include "apex_control_g4b.h"

#if IS_ENABLED(CONFIG_APEX_G4B_GAMEPAD)
#include "gamepad_g4b.h"
#else
static inline bool g4b_gamepad_is_enabled(void) { return false; }
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
extern bool zmk_usb_studio_is_on(void); /* app/src/usb.c */
#else
static inline bool zmk_usb_studio_is_on(void) { return false; }
#endif

#define FN_LAYER 1

/* USB HID keyboard usages of the keys that carry Fn functions. */
#define HID_A 0x04u
#define HID_B 0x05u
#define HID_C 0x06u
#define HID_D 0x07u
#define HID_E 0x08u
#define HID_F 0x09u
#define HID_G 0x0Au
#define HID_H 0x0Bu
#define HID_I 0x0Cu
#define HID_J 0x0Du
#define HID_K 0x0Eu
#define HID_L 0x0Fu
#define HID_M 0x10u
#define HID_N 0x11u
#define HID_O 0x12u
#define HID_P 0x13u
#define HID_Q 0x14u
#define HID_R 0x15u
#define HID_S 0x16u
#define HID_T 0x17u
#define HID_U 0x18u
#define HID_V 0x19u
#define HID_W 0x1Au
#define HID_X 0x1Bu
#define HID_Y 0x1Cu
#define HID_Z 0x1Du
#define HID_ESC   0x29u
#define HID_BSPC  0x2Au
#define HID_LBKT  0x2Fu
#define HID_RBKT  0x30u
#define HID_BSLH  0x31u
#define HID_SEMI  0x33u
#define HID_SQT   0x34u
#define HID_COMMA 0x36u
#define HID_DOT   0x37u
#define HID_FSLH  0x38u
#define HID_F1    0x3Au /* F1..F12 are contiguous 0x3A..0x45 */

/* Indicator kinds. Toggles resolve their colour from a live mode state; the
 * rest use a fixed category colour. */
enum ind_kind {
    IND_STATIC = 0,
    IND_TOGGLE_GAMEPAD,
    IND_TOGGLE_STUDIO,
    IND_TOGGLE_RT,
    IND_TOGGLE_RGB,
};

struct ind {
    uint8_t hid;
    uint8_t kind;
    uint8_t r, g, b; /* used when kind == IND_STATIC */
};

/* Category colours. */
#define C_ORANGE  255u, 90u, 0u    /* RGB hue/effect/brightness */
#define C_AMBER   255u, 150u, 0u   /* actuation controls */
#define C_MAGENTA 255u, 0u, 110u   /* media transport */
#define C_BLUE    20u, 60u, 255u   /* Bluetooth */
#define C_CYAN    0u, 190u, 190u   /* navigation / arrows */
#define C_WHITE   55u, 55u, 55u    /* F-keys */

#define TOG(hid_, kind_) { (hid_), (kind_), 0u, 0u, 0u }
#define STC(hid_, col)   { (hid_), IND_STATIC, col }

static const struct ind fn_map[] = {
    /* Mode toggles: green when on, red when off. */
    TOG(HID_Z, IND_TOGGLE_GAMEPAD),
    TOG(HID_S, IND_TOGGLE_STUDIO),
    TOG(HID_T, IND_TOGGLE_RT),
    TOG(HID_E, IND_TOGGLE_RGB),

    /* Actuation controls. */
    STC(HID_Q, C_AMBER),  /* actuation reset */
    STC(HID_I, C_AMBER),  /* shallower */
    STC(HID_O, C_AMBER),  /* deeper */
    STC(HID_X, C_AMBER),  /* effect next */

    /* RGB adjust (hue / effect / brightness). */
    STC(HID_R, C_ORANGE), /* effect prev */
    STC(HID_Y, C_ORANGE), /* hue down */
    STC(HID_U, C_ORANGE), /* hue up */
    STC(HID_C, C_ORANGE), /* brightness down */
    STC(HID_V, C_ORANGE), /* brightness up */

    /* Media transport. */
    STC(HID_B, C_MAGENTA),     /* vol down */
    STC(HID_N, C_MAGENTA),     /* vol up */
    STC(HID_M, C_MAGENTA),     /* mute */
    STC(HID_COMMA, C_MAGENTA), /* prev */
    STC(HID_DOT, C_MAGENTA),   /* next */
    STC(HID_FSLH, C_MAGENTA),  /* play/pause */

    /* Bluetooth: profiles + clear. */
    STC(HID_F, C_BLUE),
    STC(HID_G, C_BLUE),
    STC(HID_H, C_BLUE),
    STC(HID_J, C_BLUE),
    STC(HID_K, C_BLUE),
    STC(HID_BSLH, C_BLUE), /* BT clear */

    /* Navigation / arrows. */
    STC(HID_A, C_CYAN),    /* left */
    STC(HID_D, C_CYAN),    /* right */
    STC(HID_W, C_CYAN),    /* up */
    STC(HID_BSPC, C_CYAN), /* delete */
    STC(HID_P, C_CYAN),    /* print screen */
    STC(HID_LBKT, C_CYAN), /* home */
    STC(HID_RBKT, C_CYAN), /* page up */
    STC(HID_L, C_CYAN),    /* insert */
    STC(HID_SEMI, C_CYAN), /* end */
    STC(HID_SQT, C_CYAN),  /* page down */

    /* Esc (Fn = grave). */
    STC(HID_ESC, C_WHITE),
};

/* The number row, 1 .. = (12 keys, left to right), used as a battery gauge
 * while Fn is held. HID usages: 1..0 then - and =. */
static const uint8_t numrow_hid[12] = {
    0x1Eu, 0x1Fu, 0x20u, 0x21u, 0x22u, 0x23u, /* 1 2 3 4 5 6 */
    0x24u, 0x25u, 0x26u, 0x27u, 0x2Du, 0x2Eu, /* 7 8 9 0 - = */
};

static inline void set_led(uint8_t *frame, uint8_t led,
                           uint8_t r, uint8_t g, uint8_t b);

/* Paint the battery state of charge as a 12-segment bar on the number row: bar
 * length is the charge level, and the colour is the health (green / amber / red
 * as it drops). Lit segments only; the rest stay dark from the clean replace. */
static void paint_battery(uint8_t *frame)
{
    uint32_t pct = zmk_battery_state_of_charge();
    uint32_t lit;
    uint8_t r, g, b;

    if (pct > 100u) {
        pct = 100u;
    }
    lit = (pct * 12u + 50u) / 100u; /* rounded segments */
    if (lit == 0u && pct > 0u) {
        lit = 1u; /* never show an empty bar for a live battery */
    }

    if (pct > 40u) {
        r = 0u; g = 255u; b = 0u;      /* green */
    } else if (pct > 15u) {
        r = 255u; g = 150u; b = 0u;    /* amber */
    } else {
        r = 255u; g = 0u; b = 0u;      /* red */
    }

    for (uint32_t i = 0u; i < lit && i < 12u; i++) {
        uint8_t led = g4b_led_for_hid(numrow_hid[i]);

        if (led < G4B_RGB_LEDS) {
            set_led(frame, led, r, g, b);
        }
    }
}

/* Flash pool. Started from a behavior thread, read/expired on the g4b thread. */
#define FLASH_SLOTS 4u
#define FLASH_PULSE_ON_MS 150u
#define FLASH_PULSE_MS    280u /* on + gap */
#define FLASH_TOTAL_MS    560u /* two pulses */

struct flash_slot {
    uint8_t hid;
    bool green;
    bool active;
    uint32_t start;
};
static struct flash_slot flashes[FLASH_SLOTS];
static struct k_spinlock flash_lock;
static bool overlay_prev_active;

static inline void set_led(uint8_t *frame, uint8_t led,
                           uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = &frame[2u + (uint32_t)led * 3u];

    p[0] = b;
    p[1] = g;
    p[2] = r;
}

static bool toggle_on(uint8_t kind)
{
    switch (kind) {
    case IND_TOGGLE_GAMEPAD:
        return g4b_gamepad_is_enabled();
    case IND_TOGGLE_STUDIO:
        return zmk_usb_studio_is_on();
    case IND_TOGGLE_RT:
        return apex_rapid_trigger_enabled();
    case IND_TOGGLE_RGB: {
        bool on = false;
        (void)zmk_rgb_underglow_get_state(&on);
        return on;
    }
    default:
        return false;
    }
}

void g4b_rgb_overlay_flash(uint8_t hid, bool on)
{
    k_spinlock_key_t key = k_spin_lock(&flash_lock);
    uint32_t now = k_uptime_get_32();
    uint32_t slot = 0u;
    uint32_t oldest = 0u;
    uint32_t oldest_age = 0u;

    /* Reuse a slot already flashing this key, else a free slot, else the oldest. */
    for (uint32_t i = 0u; i < FLASH_SLOTS; i++) {
        if (flashes[i].active && flashes[i].hid == hid) {
            slot = i;
            goto set;
        }
    }
    for (uint32_t i = 0u; i < FLASH_SLOTS; i++) {
        if (!flashes[i].active) {
            slot = i;
            goto set;
        }
        uint32_t age = now - flashes[i].start;
        if (age >= oldest_age) {
            oldest_age = age;
            oldest = i;
        }
    }
    slot = oldest;
set:
    flashes[slot].hid = hid;
    flashes[slot].green = on;
    flashes[slot].active = true;
    flashes[slot].start = now;
    k_spin_unlock(&flash_lock, key);
}

/* Read-only: is any flash still within its window? */
static bool any_flash_active(uint32_t now)
{
    bool any = false;
    k_spinlock_key_t key = k_spin_lock(&flash_lock);

    for (uint32_t i = 0u; i < FLASH_SLOTS; i++) {
        if (flashes[i].active && (now - flashes[i].start) < FLASH_TOTAL_MS) {
            any = true;
            break;
        }
    }
    k_spin_unlock(&flash_lock, key);
    return any;
}

bool g4b_rgb_overlay_wants(uint32_t now_ms)
{
    return zmk_keymap_layer_active(FN_LAYER) || any_flash_active(now_ms);
}

/* Paint any active flashes into the frame; expire finished ones. */
static bool render_flashes(uint8_t *frame, uint32_t now)
{
    bool any = false;
    k_spinlock_key_t key = k_spin_lock(&flash_lock);

    for (uint32_t i = 0u; i < FLASH_SLOTS; i++) {
        if (!flashes[i].active) {
            continue;
        }
        uint32_t t = now - flashes[i].start;

        if (t >= FLASH_TOTAL_MS) {
            flashes[i].active = false;
            continue;
        }
        any = true;
        if (frame != NULL && (t % FLASH_PULSE_MS) < FLASH_PULSE_ON_MS) {
            uint8_t led = g4b_led_for_hid(flashes[i].hid);

            if (led < G4B_RGB_LEDS) {
                if (flashes[i].green) {
                    set_led(frame, led, 0u, 255u, 0u);
                } else {
                    set_led(frame, led, 255u, 0u, 0u);
                }
            }
        }
    }
    k_spin_unlock(&flash_lock, key);
    return any;
}

void g4b_rgb_overlay_apply(uint8_t *out_frame, uint32_t now_ms)
{
    bool fn = zmk_keymap_layer_active(FN_LAYER);

    if (fn) {
        /* Clean replace: black the array, then paint only the Fn functions. */
        memset(&out_frame[2], 0, G4B_RGB_CHANNELS);

        for (uint32_t i = 0u; i < ARRAY_SIZE(fn_map); i++) {
            uint8_t led = g4b_led_for_hid(fn_map[i].hid);

            if (led >= G4B_RGB_LEDS) {
                continue;
            }
            if (fn_map[i].kind == IND_STATIC) {
                set_led(out_frame, led, fn_map[i].r, fn_map[i].g, fn_map[i].b);
            } else if (toggle_on(fn_map[i].kind)) {
                set_led(out_frame, led, 0u, 255u, 0u); /* green: on */
            } else {
                set_led(out_frame, led, 255u, 0u, 0u); /* red: off */
            }
        }

        /* Number row 1..= becomes a battery gauge. */
        paint_battery(out_frame);
    }

    (void)render_flashes(out_frame, now_ms);
}

bool g4b_rgb_overlay_tick(uint32_t now_ms)
{
    bool active = g4b_rgb_overlay_wants(now_ms);
    bool was = overlay_prev_active;

    overlay_prev_active = active;
    /* One extra flush after deactivation so the base effect repaints. */
    return active || was;
}
