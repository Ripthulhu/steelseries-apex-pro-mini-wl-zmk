/* SPDX-License-Identifier: MIT
 *
 * apex_control - implementation. Thin wrappers over the existing subsystem
 * getters/setters (twi_g4b / actuation / rgb_fx), plus a direct nRF on-die
 * temperature read. Every subsystem call is guarded by its CONFIG so this file
 * links in any build; absent subsystems return safe defaults.
 */
#include "apex_control_g4b.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <errno.h>

#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
#include "twi_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
#include "actuation_g4b.h"
#include "mode_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
#include "rgb_fx_g4b.h"
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#include <hal/nrf_temp.h>

/* These depend on CONFIG_APEX_G4B_CHARGE_STORAGE; provide fallbacks so this file
 * compiles in a build without the storage-band controller. */
#ifndef CONFIG_APEX_G4B_CHARGE_STOP_PCT
#define CONFIG_APEX_G4B_CHARGE_STOP_PCT 0
#endif
#ifndef CONFIG_APEX_G4B_CHARGE_RESUME_PCT
#define CONFIG_APEX_G4B_CHARGE_RESUME_PCT 0
#endif

/* --------------------------------------------------------------- statistics
 *
 * Key-press count via ZMK's position-state event, off the scan path: every
 * matrix key-down increments the counter.
 */
#include <zephyr/sys/atomic.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

static atomic_t apex_keypress_count = ATOMIC_INIT(0);

/* Per-keymap-position press counts for the usage heatmap. uint16 cross-thread
 * races are acceptable here. */
static uint16_t apex_key_counts[APEX_HEATMAP_KEYS];

static int apex_stats_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->state) {
        atomic_inc(&apex_keypress_count);
        if (ev->position < APEX_HEATMAP_KEYS &&
            apex_key_counts[ev->position] < 0xFFFFu) {
            apex_key_counts[ev->position]++;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(apex_stats, apex_stats_listener);
ZMK_SUBSCRIPTION(apex_stats, zmk_position_state_changed);

uint32_t apex_stats_keypresses(void)
{
    return (uint32_t)atomic_get(&apex_keypress_count);
}

size_t apex_stats_heatmap(uint16_t *out, size_t max, uint16_t *out_peak)
{
    uint16_t peak = 0;
    size_t n = (max < APEX_HEATMAP_KEYS) ? max : (size_t)APEX_HEATMAP_KEYS;
    for (size_t i = 0; i < n; i++) {
        out[i] = apex_key_counts[i];
        if (apex_key_counts[i] > peak) {
            peak = apex_key_counts[i];
        }
    }
    if (out_peak) {
        *out_peak = peak;
    }
    return (size_t)APEX_HEATMAP_KEYS;
}

void apex_stats_heatmap_reset(void)
{
    memset(apex_key_counts, 0, sizeof(apex_key_counts));
    atomic_set(&apex_keypress_count, 0);
}

/* ------------------------------------------------- persisted control overrides
 *
 * The charge-voltage override and the mode override survive a power cycle via
 * NVS (subtree "apxc"), applied in the settings commit at boot. Debounced 5 s so
 * a burst of shell writes is one flash write. The mode override is ALSO kept in
 * NOINIT RAM by mode_g4b for the warm-reset radio-owner case; NVS adds cold-boot
 * persistence.
 */
static struct apex_ctrl_nvs {
    uint8_t version;
    int8_t mode_override; /* -1 = follow switch */
    uint16_t vreg_mv;     /* 0 = use the boot default */
} apex_ctrl = { .version = 1, .mode_override = -1, .vreg_mv = 0 };

static void apex_ctrl_save_work(struct k_work *work)
{
    ARG_UNUSED(work);
    (void)settings_save_one("apxc/s", &apex_ctrl, sizeof(apex_ctrl));
}
static K_WORK_DELAYABLE_DEFINE(apex_ctrl_dwork, apex_ctrl_save_work);

static void apex_ctrl_mark_dirty(void)
{
    (void)k_work_reschedule(&apex_ctrl_dwork, K_SECONDS(5));
}

static int apex_ctrl_set(const char *name, size_t len, settings_read_cb read_cb,
                         void *cb_arg)
{
    if (settings_name_steq(name, "s", NULL)) {
        struct apex_ctrl_nvs tmp;
        if (len != sizeof(tmp)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &tmp, sizeof(tmp));
        if (rc < 0) {
            return rc;
        }
        apex_ctrl = tmp;
        return 0;
    }
    return -ENOENT;
}

static int apex_ctrl_commit(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    if (apex_ctrl.vreg_mv != 0u) {
        (void)g4b_bq_set_vreg_mv(apex_ctrl.vreg_mv);
    }
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    if (apex_ctrl.mode_override >= 0) {
        g4b_mode_set_override(apex_ctrl.mode_override);
    }
#endif
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(apex_ctrl, "apxc", NULL, apex_ctrl_set,
                               apex_ctrl_commit, NULL);

void apex_mode_set(int mode)
{
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    g4b_mode_set_override(mode);
    apex_ctrl.mode_override = (mode >= 0 && mode <= 2) ? (int8_t)mode : -1;
    apex_ctrl_mark_dirty();
#else
    ARG_UNUSED(mode);
#endif
}

/* ------------------------------------------------------------------ battery */

bool apex_battery_read(struct apex_battery *out)
{
    if (!out) {
        return false;
    }
    *out = (struct apex_battery){0};
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    uint32_t mv = g4b_bq_sample_mv(); /* triggers one ADC conversion */
    if (mv == 0u) {
        return false;
    }
    out->millivolts = mv;
    out->percent = g4b_bq_percent(mv);
    out->charge_ma = g4b_bq_charge_ma();
    out->charging = g4b_bq_is_charging();
    out->terminated = g4b_bq_charge_terminated();
    out->power_good = g4b_bq_power_good();
    struct g4b_twi_result s = g4b_bq_read(G4B_BQ_REG_STATUS);
    struct g4b_twi_result f = g4b_bq_read(G4B_BQ_REG_FAULT);
    out->status_reg = s.ok ? s.value : 0u;
    out->fault_reg = f.ok ? f.value : 0u;
    return true;
#else
    return false;
#endif
}

/* -------------------------------------------------------- charge configuration */

void apex_charge_get(struct apex_charge_cfg *out)
{
    if (!out) {
        return;
    }
    *out = (struct apex_charge_cfg){0};
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    out->vreg_mv = g4b_bq_get_vreg_mv();
    out->ichg_ma = g4b_bq_get_ichg_ma();
    out->charging_enabled = g4b_bq_charging_enabled();
#endif
    out->stop_pct = CONFIG_APEX_G4B_CHARGE_STOP_PCT;
    out->resume_pct = CONFIG_APEX_G4B_CHARGE_RESUME_PCT;
}

bool apex_charge_set_preset(enum apex_charge_preset preset)
{
    uint16_t mv = (preset == APEX_CHARGE_100) ? APEX_VREG_100_MV : APEX_VREG_80_MV;
    return apex_charge_set_vreg_mv(mv);
}

bool apex_charge_set_vreg_mv(uint16_t mv)
{
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    if (mv < APEX_VREG_MIN_MV) {
        mv = APEX_VREG_MIN_MV;
    }
    if (mv > APEX_VREG_MAX_MV) {
        mv = APEX_VREG_MAX_MV; /* hard ceiling - never overvolt the pack */
    }
    bool ok = g4b_bq_set_vreg_mv(mv);
    if (ok) {
        apex_ctrl.vreg_mv = mv;
        apex_ctrl_mark_dirty();
    }
    return ok;
#else
    (void)mv;
    return false;
#endif
}

bool apex_charge_set_current_ma(uint16_t ma)
{
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    return g4b_bq_set_ichg_ma(ma);
#else
    (void)ma;
    return false;
#endif
}

bool apex_charge_set_enabled(bool enable)
{
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    return g4b_bq_set_charging(enable);
#else
    (void)enable;
    return false;
#endif
}

/* ---------------------------------------------------- actuation / rapid trigger */

uint8_t apex_actuation_get_tenths(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    return g4b_actuation_tenths();
#else
    return 0u;
#endif
}

bool apex_actuation_set_tenths(uint8_t tenths)
{
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    /* Discrete 6-point ladder: step toward the target, stop at a ladder bound or
     * on reaching it. */
    uint8_t cur = g4b_actuation_tenths();
    for (int guard = 0; guard < 32 && cur != tenths; guard++) {
        g4b_actuation_step(tenths > cur ? 1 : -1);
        uint8_t next = g4b_actuation_tenths();
        if (next == cur) {
            break;
        }
        cur = next;
    }
    return true;
#else
    (void)tenths;
    return false;
#endif
}

bool apex_rapid_trigger_enabled(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    return g4b_rapid_trigger_tenths() > 0u;
#else
    return false;
#endif
}

uint8_t apex_rapid_trigger_get_tenths(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    return g4b_rapid_trigger_tenths();
#else
    return 0u;
#endif
}

bool apex_rapid_trigger_set(bool enable, uint8_t tenths)
{
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    bool on = g4b_rapid_trigger_tenths() > 0u;
    if (enable && !on) {
        g4b_rapid_trigger_toggle();
    } else if (!enable && on) {
        g4b_rapid_trigger_toggle();
        return true;
    }
    if (enable) {
        uint8_t cur = g4b_rapid_trigger_tenths();
        for (int guard = 0; guard < 64 && cur != tenths; guard++) {
            g4b_rapid_trigger_step(tenths > cur ? 1 : -1);
            uint8_t next = g4b_rapid_trigger_tenths();
            if (next == cur) {
                break;
            }
            cur = next;
        }
    }
    return true;
#else
    (void)enable;
    (void)tenths;
    return false;
#endif
}

/* -------------------------------------------------------------------- rgb */

#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
static const char *const apex_fx_names[] = {
    "zmk", "plasma", "fire", "aurora", "sparkle", "ripple",
    "reactive", "analog", "shockwave", "heatmap", "rain", "ink",
};
BUILD_ASSERT(ARRAY_SIZE(apex_fx_names) == G4B_FX_COUNT,
             "apex_fx_names must match enum g4b_fx");
#endif

uint8_t apex_rgb_effect_count(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    return (uint8_t)G4B_FX_COUNT;
#else
    return 0u;
#endif
}

uint8_t apex_rgb_effect_get(void)
{
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    return (uint8_t)g4b_fx_current();
#else
    return 0u;
#endif
}

bool apex_rgb_effect_set(uint8_t index)
{
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    if (index >= G4B_FX_COUNT) {
        return false;
    }
    g4b_fx_set((enum g4b_fx)index);
    return true;
#else
    (void)index;
    return false;
#endif
}

const char *apex_rgb_effect_name(uint8_t index)
{
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    if (index >= G4B_FX_COUNT) {
        return NULL;
    }
    return apex_fx_names[index];
#else
    (void)index;
    return NULL;
#endif
}

/* Brightness lives in ZMK's underglow subsystem; not wired here - report 0/no-op. */
uint8_t apex_rgb_brightness_get(void)
{
    return 0u;
}

bool apex_rgb_brightness_set(uint8_t v)
{
    (void)v;
    return false;
}

/* -------------------------------------------------------------- telemetry */

static K_MUTEX_DEFINE(temp_lock);

static bool die_temp_read_mc(int32_t *out_mc)
{
    bool ok = false;
    k_mutex_lock(&temp_lock, K_FOREVER);
    /* The RC 32 kHz LFCLK calibration also uses the TEMP peripheral; its DATARDY
     * interrupt consumes the event before the poll sees it, so a naive read times
     * out. Mask that interrupt for the one-shot measurement, then restore it. The
     * wait is not irq_lock'd: the ~36 us busy-wait keeps radio/USB/timers live. */
    uint32_t int_was_on =
        nrf_temp_int_enable_check(NRF_TEMP, NRF_TEMP_INT_DATARDY_MASK);
    nrf_temp_int_disable(NRF_TEMP, NRF_TEMP_INT_DATARDY_MASK);
    nrf_temp_event_clear(NRF_TEMP, NRF_TEMP_EVENT_DATARDY);
    nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_START);
    for (int i = 0; i < 60; i++) { /* ~36 us typical, 1.2 ms hard cap */
        if (nrf_temp_event_check(NRF_TEMP, NRF_TEMP_EVENT_DATARDY)) {
            ok = true;
            break;
        }
        k_busy_wait(20);
    }
    if (ok) {
        nrf_temp_event_clear(NRF_TEMP, NRF_TEMP_EVENT_DATARDY);
        int32_t raw = nrf_temp_result_get(NRF_TEMP); /* 0.25 degC steps, signed */
        *out_mc = raw * 250;
    }
    nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_STOP);
    if (int_was_on) {
        nrf_temp_int_enable(NRF_TEMP, NRF_TEMP_INT_DATARDY_MASK);
    }
    k_mutex_unlock(&temp_lock);
    return ok;
}

bool apex_telemetry_read(struct apex_telemetry *out)
{
    if (!out) {
        return false;
    }
    *out = (struct apex_telemetry){0};

    int32_t mc = 0;
    out->die_temp_valid = die_temp_read_mc(&mc);
    out->die_temp_mc = mc;

#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    /* One-shot ADC conversion so REG10 (TS) and REG0E are fresh, then read them. */
    (void)g4b_bq_sample_mv();
    struct g4b_twi_result ts = g4b_bq_read(0x10u); /* BQ REG10 TSPCT */
    struct g4b_twi_result batv = g4b_bq_read(G4B_BQ_REG_BATV); /* THERM_STAT bit7 */
    if (ts.ok) {
        out->bq_ts_pct = (uint8_t)(ts.value & 0x7Fu);
    }
    out->bq_therm_regulating = batv.ok && (batv.value & 0x80u);
    int16_t bt = g4b_bq_ts_temp_c();
    out->batt_temp_valid = (bt != G4B_BQ_TEMP_INVALID);
    out->batt_temp_c = bt;
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
    out->usb_conn_state = (uint8_t)zmk_usb_get_conn_state();
#endif
    out->uptime_s = (uint32_t)(k_uptime_get() / 1000);
    out->keypresses = apex_stats_keypresses();
    return true;
}
