/* SPDX-License-Identifier: MIT
 *
 * apex_control - the single control/telemetry API for the Apex Pro Mini WL.
 *
 * This is the shared layer that the three host-facing frontends all call, so a
 * value changed from the UART shell, ZMK Studio (apex RPC subsystem), or a Fn
 * keymap behavior goes through identical, already-thread-safe code and persists
 * the same way (NVS via the existing settings records):
 *
 *     [ UART shell ]      [ Studio apex RPC ]      [ keymap behaviors ]
 *              \                  |                        /
 *               \                 v                       /
 *                +--------> apex_control API <-----------+
 *                          (this header)
 *                                 |
 *          +----------------------+-----------------------+
 *          v                      v                       v
 *   twi_g4b (BQ25895)     actuation_g4b (STM32)     rgb_fx_g4b (IS31FL3743B)
 *
 * Getters are cheap/non-blocking where possible; battery reads trigger one
 * BQ ADC conversion. Setters clamp to safe bounds and mark settings dirty for
 * the debounced NVS write. NOTHING here can reach BQ REG00 or STM32 opcode 0x32
 * (flash calibration) - both stay off-limits by construction.
 */
#ifndef APEX_CONTROL_G4B_H
#define APEX_CONTROL_G4B_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ battery */

struct apex_battery {
    uint32_t millivolts;       /* last BQ ADC conversion */
    uint32_t percent;          /* pack curve, scaled to active ceiling */
    uint32_t charge_ma;        /* 0 unless actively charging */
    bool     charging;         /* current flowing into the cell */
    bool     terminated;       /* topped to the VREG cap */
    bool     power_good;       /* input present (plugged in) */
    uint8_t  status_reg;       /* BQ REG0B raw */
    uint8_t  fault_reg;        /* BQ REG0C raw */
};

/* Sample the charger (one ADC conversion) and fill @out. Returns false on I2C
 * failure, in which case @out is zeroed. */
bool apex_battery_read(struct apex_battery *out);

/* ------------------------------------------------------- charge configuration
 *
 * Live BQ25895 config, bounded. VREG (charge-voltage ceiling) is HARD-CLAMPED to
 * APEX_VREG_MIN_MV..APEX_VREG_MAX_MV; the pack (Fuji 4867A0) is rated 4.400 V, so
 * the ceiling can never be driven into overvoltage. "80% / 100%" are presets.
 */
#define APEX_VREG_MIN_MV   3840u   /* BQ25895 floor */
#define APEX_VREG_MAX_MV   4400u   /* pack rated max - NEVER above this */
#define APEX_VREG_80_MV    4096u   /* longevity ceiling (current default) */
#define APEX_VREG_100_MV   4352u   /* "full" preset, still under 4.400 V */

enum apex_charge_preset { APEX_CHARGE_80 = 80, APEX_CHARGE_100 = 100 };

struct apex_charge_cfg {
    uint16_t vreg_mv;          /* active charge-voltage ceiling */
    uint16_t ichg_ma;          /* charge current limit */
    bool     charging_enabled; /* REG03 CHG_CONFIG */
    uint8_t  stop_pct;         /* storage-band hold ceiling */
    uint8_t  resume_pct;       /* storage-band resume floor */
};

void apex_charge_get(struct apex_charge_cfg *out);
/* Apply an 80% / 100% preset (sets vreg_mv to the matching constant). */
bool apex_charge_set_preset(enum apex_charge_preset preset);
/* Set the charge-voltage ceiling directly, clamped to [MIN,MAX]. */
bool apex_charge_set_vreg_mv(uint16_t mv);
/* Set the charge current limit, clamped to the BQ/pack safe range. */
bool apex_charge_set_current_ma(uint16_t ma);
/* Enable/disable charging (passthrough when disabled). */
bool apex_charge_set_enabled(bool enable);

/* ---------------------------------------------------- actuation / rapid trigger
 *
 * Global (not per-key). Values in tenths of a millimetre. The ladder is 6
 * measured points 1.0..3.0 mm; set() snaps to the nearest and steps the STM32.
 */
uint8_t apex_actuation_get_tenths(void);
bool    apex_actuation_set_tenths(uint8_t tenths);   /* snaps to ladder */

bool    apex_rapid_trigger_enabled(void);
uint8_t apex_rapid_trigger_get_tenths(void);         /* 0 when off */
bool    apex_rapid_trigger_set(bool enable, uint8_t tenths);

/* -------------------------------------------------------------------- rgb */

uint8_t     apex_rgb_effect_count(void);
uint8_t     apex_rgb_effect_get(void);
bool        apex_rgb_effect_set(uint8_t index);
const char *apex_rgb_effect_name(uint8_t index);     /* NULL if out of range */
uint8_t     apex_rgb_brightness_get(void);           /* 0..255 (ZMK underglow) */
bool        apex_rgb_brightness_set(uint8_t v);

/* -------------------------------------------------------------- telemetry */

struct apex_telemetry {
    int32_t  die_temp_mc;      /* nRF52833 on-die temp, milli-degC */
    bool     die_temp_valid;
    int16_t  batt_temp_c;      /* battery pack temp from the BQ TS-pin NTC, degC */
    bool     batt_temp_valid;
    uint8_t  bq_ts_pct;        /* BQ REG10 TS voltage as % of REGN (raw) */
    bool     bq_therm_regulating; /* BQ REG0E THERM_STAT */
    uint8_t  usb_conn_state;   /* zmk_usb_get_conn_state(): NONE/POWERED/HID */
    uint32_t uptime_s;
    uint32_t keypresses;       /* total key presses since boot */
};

bool apex_telemetry_read(struct apex_telemetry *out);

/* Total key presses counted since boot (via a ZMK position-state listener). */
uint32_t apex_stats_keypresses(void);

/* Usage heatmap: per-keymap-position press counts. Studio maps position -> key. */
#define APEX_HEATMAP_KEYS 128u
/* Copy up to @max counts into @out; @out_peak (optional) gets the max count.
 * Returns the true key count. */
size_t apex_stats_heatmap(uint16_t *out, size_t max, uint16_t *out_peak);
void   apex_stats_heatmap_reset(void);

/* Force the transport mode (a g4b_mode value, or -1 to follow the switch) AND
 * persist it to NVS so it survives a power cycle. Wraps g4b_mode_set_override. */
void apex_mode_set(int mode);

#endif /* APEX_CONTROL_G4B_H */
