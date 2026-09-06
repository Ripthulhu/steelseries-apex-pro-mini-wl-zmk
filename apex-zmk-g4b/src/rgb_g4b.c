/* SPDX-License-Identifier: MIT
 *
 * IS31FL3743B RGB driver on SPIM2, direct-register in the same style as the
 * STM32 link on SPIM3. See rgb_g4b.h for the bus map.
 *
 * Register facts are the vendor's, read from the stock image:
 *   - write path 0x293C0 (PWM, command 0x50) and 0x29414 (scaling, 0x51):
 *       CS low, { command, 1-based register, data... }, CS high
 *   - function page (command 0x52): reg 0x00 Config, 0x01 Global Current,
 *       0x02 Pull-Up/Down. These register bytes are literal, not 1-based.
 *   - stock function-page values: Config 0x09 (SSD=1, normal run), Global
 *       Current 0xFF, Pull 0x33.
 *   - SPIM2 FREQUENCY left by the loader is 0x40000000 = M4 (4 Mbit/s), CONFIG
 *       0 (mode 0, MSB first). Used verbatim rather than pushed to 8 Mbit/s.
 *
 * The chip auto-increments its register pointer within a page, so a full PWM or
 * scaling page is one CS-framed transfer of {command, 0x01, 198 bytes} rather
 * than 66 per-LED writes. Electrically identical to stock; simpler here.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <nrfx.h>

#include "pins_g4b.h"
#include "rgb_g4b.h"
#if IS_ENABLED(CONFIG_APEX_G4B_FN_OVERLAY)
#include "rgb_overlay_g4b.h"
#endif

#define G4B_RGB_END_US   20000u

/* PSEL encodes the port in bit 5: pin | (port << 5). */
#define G4B_RGB_PSEL_SCK  (9u | (1u << 5))  /* P1.09 -> 0x29 */
#define G4B_RGB_PSEL_MOSI (8u | (1u << 5))  /* P1.08 -> 0x28 */

#define G4B_RGB_CMD_PWM   0x50u
#define G4B_RGB_CMD_SCALE 0x51u
#define G4B_RGB_CMD_FUNC  0x52u

#define G4B_RGB_FN_CONFIG   0x00u
#define G4B_RGB_FN_GCURRENT 0x01u
#define G4B_RGB_FN_PULL     0x02u

#define G4B_RGB_CONFIG_RUN  0x09u
/* Stock global-current setting. */
#define G4B_RGB_GCURRENT    0xFFu
#define G4B_RGB_PULL        0x33u

/* Frame buffer plus the 2-byte page header, all in RAM: EasyDMA cannot read
 * flash. The header sits in the same buffer so the whole page goes out under
 * one CS assertion in a single transfer.
 */
static uint8_t rgb_pwm_frame[2u + G4B_RGB_CHANNELS];

/* Global fade for the backlight idle timeout. This scales the whole frame by
 * rgb_fade (0..256,
 * 256 = full) on the way out, and g4b_rgb_idle_tick() ramps that scalar over
 * ~10 s instead, so the backlight eases out and back in. The rail is only cut
 * once the fade reaches black - the power saving is kept, just deferred to the
 * end of the fade. rgb_out_frame is a separate scaled copy so the staged frame
 * is never disturbed and a return to full brightness is exact.
 */
#define G4B_RGB_FADE_OUT_MS 10000u
#define G4B_RGB_FADE_IN_MS  2000u

static uint16_t rgb_fade = 256u;        /* current scalar, 0..256 */
static uint16_t rgb_fade_target = 256u; /* where the ramp is heading */
static uint16_t rgb_fade_from = 256u;   /* level the current ramp started at */
static uint32_t rgb_fade_t0;            /* uptime ms the current ramp started */
static uint8_t rgb_out_frame[2u + G4B_RGB_CHANNELS];
/* Per-channel dither residue for the fade. Scaling an 8-bit channel down to a
 * dim level leaves only a handful of distinct PWM codes, so a plain multiply
 * banded visibly on the way out. Carrying the sub-code remainder to the next
 * frame (temporal dither, exactly as the effect engine does) makes the average
 * land between codes and the fade reads smooth. The g4b thread flushes fast
 * while a fade is in flight, so the dither runs well above visible flicker.
 */
static uint8_t rgb_fade_resid[G4B_RGB_CHANNELS];

static void spim2_enable(void)
{
    NRF_SPIM2->PSEL.SCK = G4B_RGB_PSEL_SCK;
    NRF_SPIM2->PSEL.MOSI = G4B_RGB_PSEL_MOSI;
    NRF_SPIM2->PSEL.MISO = 0xFFFFFFFFu; /* write-only device */
    NRF_SPIM2->PSEL.CSN = 0xFFFFFFFFu;  /* CS is bit-banged on P0.11 */
    NRF_SPIM2->FREQUENCY = 0x40000000u; /* M4, exactly as the loader left it */
    NRF_SPIM2->CONFIG = 0u;             /* mode 0, MSB first */
    NRF_SPIM2->ORC = 0u;
    NRF_SPIM2->SHORTS = 0u;
    NRF_SPIM2->INTENCLR = 0xFFFFFFFFu;  /* fully polled */
    NRF_SPIM2->ENABLE = 7u;
    __DSB();
}

static void spim2_park(void)
{
    /* Every transfer is polled to completion on the g4b thread, and this runs
     * only after the final dark-page transfer. Stop defensively, disable the
     * peripheral, then disconnect its pins. P1.08/P1.09 already have their
     * input buffers disconnected, so removing PSEL leaves both high-Z. */
    NRF_SPIM2->TASKS_STOP = 1u;
    NRF_SPIM2->ENABLE = 0u;
    NRF_SPIM2->PSEL.SCK = 0xFFFFFFFFu;
    NRF_SPIM2->PSEL.MOSI = 0xFFFFFFFFu;
    NRF_SPIM2->PSEL.MISO = 0xFFFFFFFFu;
    NRF_SPIM2->PSEL.CSN = 0xFFFFFFFFu;
    __DSB();
}

/* One CS-framed, write-only transfer. Bounded like every wait in this payload:
 * a hang here would otherwise be recovered only by the watchdog, and a bounded
 * wait fails loudly instead of silently.
 */
static void spim2_write(const uint8_t *buf, uint32_t len)
{
    uint32_t start;

    g4b_rgb_cs_low();

    NRF_SPIM2->TXD.PTR = (uint32_t)buf;
    NRF_SPIM2->TXD.MAXCNT = len;
    NRF_SPIM2->RXD.PTR = 0u;
    NRF_SPIM2->RXD.MAXCNT = 0u;
    NRF_SPIM2->TXD.LIST = 0u;
    NRF_SPIM2->RXD.LIST = 0u;
    NRF_SPIM2->EVENTS_END = 0u;
    (void)NRF_SPIM2->EVENTS_END;

    NRF_SPIM2->TASKS_START = 1u;

    /* Use the kernel cycle counter because this POST_KERNEL initialization runs
     * before the scanner thread enables DWT CYCCNT. This also avoids a link-time
     * dependency on the optional evidence module. */
    start = k_cycle_get_32();
    while (NRF_SPIM2->EVENTS_END == 0u) {
        if (k_cyc_to_us_floor32(k_cycle_get_32() - start) > G4B_RGB_END_US) {
            break;
        }
    }

    g4b_rgb_cs_high();
}

/* A function-page register write: {0x52, reg, value}. */
static void rgb_func_write(uint8_t reg, uint8_t value)
{
    uint8_t frame[3] = { G4B_RGB_CMD_FUNC, reg, value };

    spim2_write(frame, sizeof(frame));
}

#if IS_ENABLED(CONFIG_APEX_G4B_RGB_READBACK)
/* One CS-framed full-duplex read. Sends {read command, start register} then
 * clocks `count` dummy bytes; the controller drives its data on MISO from the
 * third byte on, auto-incrementing the register pointer. out[] receives the
 * `count` data bytes. read_cmd is the write command with D7 set (e.g. 0xD2 for
 * the function page). Needs PSEL.MISO connected by the caller. */
#define G4B_RGB_READ_MAX 40u
static uint8_t rgb_read_tx[2u + G4B_RGB_READ_MAX];
static uint8_t rgb_read_rx[2u + G4B_RGB_READ_MAX];

static void spim2_read(uint8_t read_cmd, uint8_t start_reg, uint8_t *out,
                       uint32_t count)
{
    uint32_t start;
    uint32_t len = 2u + count;

    if (count > G4B_RGB_READ_MAX) {
        return;
    }
    rgb_read_tx[0] = read_cmd;
    rgb_read_tx[1] = start_reg;
    memset(&rgb_read_tx[2], 0, count);
    memset(rgb_read_rx, 0, len);

    g4b_rgb_cs_low();

    NRF_SPIM2->TXD.PTR = (uint32_t)rgb_read_tx;
    NRF_SPIM2->TXD.MAXCNT = len;
    NRF_SPIM2->RXD.PTR = (uint32_t)rgb_read_rx;
    NRF_SPIM2->RXD.MAXCNT = len;
    NRF_SPIM2->TXD.LIST = 0u;
    NRF_SPIM2->RXD.LIST = 0u;
    NRF_SPIM2->EVENTS_END = 0u;
    (void)NRF_SPIM2->EVENTS_END;

    NRF_SPIM2->TASKS_START = 1u;

    start = k_cycle_get_32();
    while (NRF_SPIM2->EVENTS_END == 0u) {
        if (k_cyc_to_us_floor32(k_cycle_get_32() - start) > G4B_RGB_END_US) {
            break;
        }
    }

    g4b_rgb_cs_high();

    /* Data begins after the command and address bytes. */
    memcpy(out, &rgb_read_rx[2], count);
}
#endif /* CONFIG_APEX_G4B_RGB_READBACK */

/* Fill a whole 198-register page (PWM or scaling) with one value. */
static void rgb_page_fill(uint8_t command, uint8_t value)
{
    static uint8_t frame[2u + G4B_RGB_CHANNELS];

    frame[0] = command;
    frame[1] = 0x01u; /* data registers are 1-based on PWM and scaling pages */
    memset(&frame[2], value, G4B_RGB_CHANNELS);

    spim2_write(frame, sizeof(frame));
}

/* Full cold bring-up of the controller. Callable any number of times.
 *
 * Separated from g4b_rgb_init() because it is now needed on every rail rise,
 * not only at boot: the IS31FL3743B loses its entire register state while
 * P0.19 is down, which is exactly why stock re-initialises all 396 registers
 * whenever it raises that pin.
 *
 * Order: zero the PWM page and write scaling while the part is still in
 * software shutdown, then leave shutdown and set the current reference. This
 * prevents stale PWM state from becoming visible after a rail cycle.
 */
/* Master current scale (function reg 0x01) and per-colour scaling balance
 * (scaling page 0x51), remembered across the idle rail cycle so bring-up
 * re-applies them after the controller loses state. Balance is a global white/
 * tint trim: each colour's scaling multiplies its PWM current, so lowering one
 * shifts the whole board's colour without touching any effect. */
static uint8_t rgb_gcurrent = G4B_RGB_GCURRENT;
static uint8_t rgb_scale_r = 0xFFu;
static uint8_t rgb_scale_g = 0xFFu;
static uint8_t rgb_scale_b = 0xFFu;

/* Write the scaling page as a repeating per-LED [B, G, R] pattern (the order the
 * controller stores each LED in), applying the global colour balance. */
static void rgb_scale_apply(void)
{
    static uint8_t frame[2u + G4B_RGB_CHANNELS];

    frame[0] = G4B_RGB_CMD_SCALE;
    frame[1] = 0x01u;
    for (uint32_t led = 0u; led < G4B_RGB_LEDS; led++) {
        uint8_t *p = &frame[2u + led * 3u];
        p[0] = rgb_scale_b;
        p[1] = rgb_scale_g;
        p[2] = rgb_scale_r;
    }
    spim2_write(frame, sizeof(frame));
}

void g4b_rgb_bringup(void)
{
    rgb_page_fill(G4B_RGB_CMD_PWM, 0x00u);
    rgb_scale_apply();
    rgb_func_write(G4B_RGB_FN_PULL, G4B_RGB_PULL);
    rgb_func_write(G4B_RGB_FN_CONFIG, G4B_RGB_CONFIG_RUN);
    rgb_func_write(G4B_RGB_FN_GCURRENT, rgb_gcurrent);
}

void g4b_rgb_set_balance(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_scale_r = r;
    rgb_scale_g = g;
    rgb_scale_b = b;
    if (!g4b_rgb_is_blanked()) {
        rgb_scale_apply();
    }
}

void g4b_rgb_get_balance(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = rgb_scale_r;
    *g = rgb_scale_g;
    *b = rgb_scale_b;
}

void g4b_rgb_set_gcurrent(uint8_t value)
{
    rgb_gcurrent = value;
    /* Apply now if the bus is live; if the array is idle-blanked the rail is
     * down and the value is picked up by the next bring-up. */
    if (!g4b_rgb_is_blanked()) {
        rgb_func_write(G4B_RGB_FN_GCURRENT, value);
    }
}

uint8_t g4b_rgb_get_gcurrent(void)
{
    return rgb_gcurrent;
}

#if IS_ENABLED(CONFIG_APEX_G4B_RGB_READBACK)
/* Function-page read command: write command (0x52) with the R/W bit (D7) set. */
#define G4B_RGB_READ_FUNC 0xD2u
/* Configuration-register values with the open/short-detect enable bits set.
 * D3 must be 1, SSD (D0) = 1 for normal operation, OSDE (D2:D1) selects the
 * mode: 01 = open, 10 = short. */
#define G4B_RGB_CONFIG_OPEN  0x0Bu /* 0000 1 01 1 */
#define G4B_RGB_CONFIG_SHORT 0x0Du /* 0000 1 10 1 */
/* Detection prerequisites (datasheet OPEN/SHORT DETECT: case 1). */
#define G4B_RGB_OSD_GCC   0x0Fu
#define G4B_RGB_OSD_PULL  0x00u
#define G4B_RGB_OSD_FIRST 0x03u /* first open/short result register */
#define G4B_RGB_FN_TEMP   0x24u /* thermal status register */
#define G4B_RGB_SENTINEL  0x5Au /* distinctive value for the wiring proof */

static void rgb_detect_pass(uint8_t config_mode, uint8_t *out)
{
    /* One-off trigger: OSDE must go 00 -> mode (clear before set). */
    rgb_func_write(G4B_RGB_FN_CONFIG, G4B_RGB_CONFIG_RUN);
    rgb_func_write(G4B_RGB_FN_CONFIG, config_mode);
    k_msleep(3); /* datasheet: valid after >= 2 scan cycles (~66 us) */
    spim2_read(G4B_RGB_READ_FUNC, G4B_RGB_OSD_FIRST, out, G4B_RGB_OSD_REGS);
}

void g4b_rgb_readback_run(struct g4b_rgb_readback *out)
{
    uint8_t known[3] = {0};

    memset(out, 0, sizeof(*out));

    g4b_rgb_miso_enable();
    NRF_SPIM2->PSEL.MISO = 8u; /* P0.08 */
    __DSB();

    /* Snapshot of the three configured function registers and thermal status. */
    spim2_read(G4B_RGB_READ_FUNC, G4B_RGB_FN_CONFIG, known, 3u);
    out->cfg = known[0];
    out->gcc = known[1];
    out->pull = known[2];
    spim2_read(G4B_RGB_READ_FUNC, G4B_RGB_FN_TEMP, &out->temp, 1u);

    /* Definitive wiring proof: write a distinctive value and read it back. A
     * healthy board has no open LEDs, so the open registers alone cannot tell a
     * wired-but-clean result from a floating-low MISO - the sentinel can. */
    rgb_func_write(G4B_RGB_FN_GCURRENT, G4B_RGB_SENTINEL);
    spim2_read(G4B_RGB_READ_FUNC, G4B_RGB_FN_GCURRENT, &out->sentinel, 1u);
    out->wired = (out->sentinel == G4B_RGB_SENTINEL);

    if (out->wired) {
        /* Light every channel at full current so each one refreshes: off/low
         * dots are not reliably scanned for open/short. PWM and scaling both to
         * full, independent of the user's colour balance, or a dimmed channel
         * reads as not-open. Dim GCC (0x0F) and brief, so it is a faint flash. */
        rgb_page_fill(G4B_RGB_CMD_PWM, 0xFFu);
        rgb_page_fill(G4B_RGB_CMD_SCALE, 0xFFu);
        rgb_func_write(G4B_RGB_FN_PULL, G4B_RGB_OSD_PULL);
        rgb_func_write(G4B_RGB_FN_GCURRENT, G4B_RGB_OSD_GCC);

        rgb_detect_pass(G4B_RGB_CONFIG_OPEN, out->open);
        rgb_detect_pass(G4B_RGB_CONFIG_SHORT, out->shorted);
    }

    /* Restore the controller and repaint the real frame on the next flush. */
    NRF_SPIM2->PSEL.MISO = 0xFFFFFFFFu;
    __DSB();
    g4b_rgb_miso_disable();
    g4b_rgb_bringup();
    g4b_rgb_mark_pending();
}
#endif /* CONFIG_APEX_G4B_RGB_READBACK */

int g4b_rgb_init(void)
{
    g4b_rgb_cs_init();
    spim2_enable();

    rgb_pwm_frame[0] = G4B_RGB_CMD_PWM;
    rgb_pwm_frame[1] = 0x01u;
    g4b_rgb_set_all(0u, 0u, 0u);

    g4b_rgb_bringup();

    return 0;
}

void g4b_rgb_set_pixel(uint8_t led, uint8_t r, uint8_t g, uint8_t b)
{
    if (led >= G4B_RGB_LEDS) {
        return;
    }

    /* The controller stores each LED as B, G, R. */
    uint8_t *px = &rgb_pwm_frame[2u + (uint32_t)led * 3u];

    px[0] = b;
    px[1] = g;
    px[2] = r;
}

/* Write the three registers of an LED's triple directly, with NO colour
 * interpretation. Used by the mapping probe: the caller is naming register
 * offsets, not colours, which is exactly what has to be established.
 */
void g4b_rgb_set_raw(uint8_t led, uint8_t reg0, uint8_t reg1, uint8_t reg2)
{
    if (led >= G4B_RGB_LEDS) {
        return;
    }
    uint8_t *px = &rgb_pwm_frame[2u + (uint32_t)led * 3u];

    px[0] = reg0;
    px[1] = reg1;
    px[2] = reg2;
}

void g4b_rgb_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint8_t i = 0u; i < G4B_RGB_LEDS; i++) {
        g4b_rgb_set_pixel(i, r, g, b);
    }
}

void g4b_rgb_show(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_FN_OVERLAY)
    uint32_t now = k_uptime_get_32();
    bool overlay = g4b_rgb_overlay_wants(now);
#else
    const bool overlay = false;
#endif

    /* Full brightness with no overlay is the common case and needs no copy:
     * send the staged frame straight out. Otherwise scale every channel by the
     * fade (or copy it), composite the overlay onto the copy, and send that -
     * always leaving the staged frame untouched so the base effect survives.
     */
    if (rgb_fade >= 256u && !overlay) {
        spim2_write(rgb_pwm_frame, sizeof(rgb_pwm_frame));
        return;
    }

    rgb_out_frame[0] = rgb_pwm_frame[0];
    rgb_out_frame[1] = rgb_pwm_frame[1];
    if (rgb_fade >= 256u) {
        memcpy(&rgb_out_frame[2], &rgb_pwm_frame[2], G4B_RGB_CHANNELS);
    } else {
        for (uint32_t i = 0u; i < G4B_RGB_CHANNELS; i++) {
            /* channel*fade is in 1/256 units; emit the whole part plus a
             * dithered carry so the low byte is not just thrown away. */
            uint32_t prod = (uint32_t)rgb_pwm_frame[2u + i] * rgb_fade;
            uint32_t whole = prod >> 8;
            uint32_t acc = (uint32_t)rgb_fade_resid[i] + (prod & 0xFFu);

            if (acc >= 256u) {
                acc -= 256u;
                whole++;
            }
            rgb_fade_resid[i] = (uint8_t)acc;
            rgb_out_frame[2u + i] = (uint8_t)(whole > 255u ? 255u : whole);
        }
    }

#if IS_ENABLED(CONFIG_APEX_G4B_FN_OVERLAY)
    if (overlay) {
        g4b_rgb_overlay_apply(rgb_out_frame, now);
    }
#endif
    spim2_write(rgb_out_frame, sizeof(rgb_out_frame));
}

/* --- Deferred transmit --------------------------------------------------
 *
 * ZMK's underglow workqueue stages pixels and the scanner thread transmits them
 * through g4b_rgb_flush(). SPIM2 and the software-controlled P0.11 chip select
 * therefore have one owner. Add serialization before introducing a transmit
 * path from any other thread. */
static volatile uint8_t rgb_pending;
static bool rgb_blanked;

void g4b_rgb_mark_pending(void)
{
    rgb_pending = 1u;
}

void g4b_rgb_flush(void)
{
    if (rgb_pending == 0u) {
        return;
    }
    rgb_pending = 0u;

    /* Staged, then dropped. The pending flag is still cleared so a frame does
     * not sit queued for however long the blank lasts and then flash up stale
     * on unblank - the next tick restages a current one within 20 ms anyway.
     */
    if (rgb_blanked) {
        return;
    }
    g4b_rgb_show();
}

void g4b_rgb_set_blanked(bool blank)
{
    if (blank == rgb_blanked) {
        return;
    }
    rgb_blanked = blank;

    if (blank) {
        /* A separate all-zero page rather than zeroing rgb_pwm_frame, so the
         * staged frame survives untouched and unblanking is instant.
         *
         * Pushed BEFORE the rail drops, while the controller is still powered.
         * One transfer; the array goes dark by command rather than by losing its
         * supply, so no bright frame flashes as the rail collapses.
         */
        static uint8_t dark[2u + G4B_RGB_CHANNELS];

        dark[0] = G4B_RGB_CMD_PWM;
        dark[1] = 0x01u;
        memset(&dark[2], 0, G4B_RGB_CHANNELS);
        spim2_write(dark, sizeof(dark));

        spim2_park();
        g4b_rgb_rail_down();
        g4b_rgb_cs_park();
    } else {
        /* Restore CS and SPIM before the rail rises, then do a full controller
         * bring-up (all controller state is lost with the supply). Re-arm the
         * pending flag last, so the next flush repaints the staged frame within
         * one 50 ms tick.
         *
         * Rails and blank are one piece of state: two independent "LEDs off"
         * flags that can disagree risk leaving the array permanently dark.
         */
        g4b_rgb_cs_init();
        spim2_enable();
        g4b_rgb_rail_up();
        g4b_rgb_bringup();
        rgb_pending = 1u;
    }
}

bool g4b_rgb_fading(void)
{
    return rgb_fade != rgb_fade_target;
}

bool g4b_rgb_is_blanked(void)
{
    return rgb_blanked;
}

/* Drive the backlight fade one step toward on (want_on) or off, using wall
 * clock so the result is independent of how fast this loop happens to tick.
 * Called from the keyboard loop just before g4b_rgb_flush(), on the g4b thread,
 * so it may touch the rail and the frame like the rest of this file.
 */
void g4b_rgb_idle_tick(bool want_on, uint32_t now)
{
    uint16_t target = want_on ? 256u : 0u;
    uint32_t dur, elapsed, prog;

    /* Coming back from a fully blanked (rail down) state: raise the rail, then
     * fade UP from black rather than snapping to full. */
    if (want_on && rgb_blanked) {
        g4b_rgb_set_blanked(false);
        rgb_fade = 0u;
        rgb_fade_target = 256u;
        rgb_fade_from = 0u;
        rgb_fade_t0 = now;
    } else if (target != rgb_fade_target) {
        /* Direction changed (or a fade is starting): snapshot where and when so
         * the ramp below is measured from here. */
        rgb_fade_target = target;
        rgb_fade_from = rgb_fade;
        rgb_fade_t0 = now;
    }

    if (rgb_fade == rgb_fade_target) {
        /* Settled dark with the rail still up: cut it now for the power saving
         * the blanker exists to provide. */
        if (rgb_fade_target == 0u && !rgb_blanked) {
            g4b_rgb_set_blanked(true);
        }
        return;
    }

    dur = (rgb_fade_target > rgb_fade_from) ? G4B_RGB_FADE_IN_MS
                                            : G4B_RGB_FADE_OUT_MS;
    elapsed = now - rgb_fade_t0;
    prog = (elapsed >= dur) ? 256u : (elapsed * 256u) / dur;

    if (rgb_fade_target >= rgb_fade_from) {
        rgb_fade = (uint16_t)(rgb_fade_from +
                   ((uint32_t)(rgb_fade_target - rgb_fade_from) * prog) / 256u);
    } else {
        rgb_fade = (uint16_t)(rgb_fade_from -
                   ((uint32_t)(rgb_fade_from - rgb_fade_target) * prog) / 256u);
    }

    rgb_pending = 1u; /* re-transmit with the new scalar on the next flush */

    if (rgb_fade == 0u && rgb_fade_target == 0u && !rgb_blanked) {
        g4b_rgb_set_blanked(true);
    }
}

/* The LEDs are exposed as a ZMK underglow device. The led_strip shim in
 * led_strip_g4b.c calls g4b_rgb_init() from device init and drives frames through
 * g4b_rgb_set_pixel/show; ZMK owns the effects and Fn-layer controls.
 */
