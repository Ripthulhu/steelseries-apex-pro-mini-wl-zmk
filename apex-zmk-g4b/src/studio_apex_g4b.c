/* SPDX-License-Identifier: MIT
 *
 * The `apex` ZMK Studio RPC subsystem: heatmap, status, and per-key actuation
 * over the same protobuf RPC transport as core/keymap/behaviors. Wrappers over
 * the shared apex_control / actuation APIs.
 */
#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO)

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/studio/rpc.h>

#include "apex_control_g4b.h"
#include "actuation_g4b.h" /* per-key actuation API */
#include "kscan_g4b.h"     /* APEX_G4B_KEY_COUNT */

ZMK_RPC_SUBSYSTEM(apex)

#define APEX_RESPONSE(type, ...) ZMK_RPC_RESPONSE(apex, type, __VA_ARGS__)

zmk_studio_Response apex_get_heatmap(const zmk_studio_Request *req)
{
    ARG_UNUSED(req);
    zmk_apex_Heatmap hm = zmk_apex_Heatmap_init_zero;
    uint16_t counts16[APEX_HEATMAP_KEYS];
    uint16_t peak = 0;
    size_t n = apex_stats_heatmap(counts16, APEX_HEATMAP_KEYS, &peak);

    if (n > 128u) {
        n = 128u; /* proto max_count */
    }
    uint32_t total = 0;
    for (size_t i = 0; i < n; i++) {
        hm.counts[i] = counts16[i];
        total += counts16[i];
    }
    hm.counts_count = (pb_size_t)n;
    hm.total = total;
    hm.peak = peak;

    return APEX_RESPONSE(apex_get_heatmap, hm);
}

zmk_studio_Response apex_reset_heatmap(const zmk_studio_Request *req)
{
    ARG_UNUSED(req);
    apex_stats_heatmap_reset();
    return APEX_RESPONSE(apex_reset_heatmap, true);
}

zmk_studio_Response apex_get_status(const zmk_studio_Request *req)
{
    ARG_UNUSED(req);
    zmk_apex_Status st = zmk_apex_Status_init_zero;

    struct apex_battery b;
    if (apex_battery_read(&b)) {
        st.battery_mv = b.millivolts;
        st.battery_pct = b.percent;
        st.charging = b.charging;
    }

    struct apex_telemetry t;
    apex_telemetry_read(&t);
    st.batt_temp_valid = t.batt_temp_valid;
    st.batt_temp_c = t.batt_temp_valid ? t.batt_temp_c : 0;
    st.die_temp_valid = t.die_temp_valid;
    st.die_temp_dc = t.die_temp_valid ? (t.die_temp_mc / 100) : 0;

    return APEX_RESPONSE(apex_get_status, st);
}

zmk_studio_Response apex_get_actuation(const zmk_studio_Request *req)
{
    ARG_UNUSED(req);
    zmk_apex_Actuation act = zmk_apex_Actuation_init_zero;

    act.global_tenths = apex_actuation_get_tenths();
    act.per_key_tenths_count = (pb_size_t)APEX_G4B_KEY_COUNT;
    for (uint32_t k = 0u; k < APEX_G4B_KEY_COUNT; k++) {
        /* 0 where the key follows the global point, else its override depth. */
        act.per_key_tenths[k] = g4b_act_key_is_override(k) ? g4b_act_key_tenths(k) : 0u;
    }

    return APEX_RESPONSE(apex_get_actuation, act);
}

zmk_studio_Response apex_set_key_actuation(const zmk_studio_Request *req)
{
    const zmk_apex_SetKeyActuation *s =
        &req->subsystem.apex.request_type.apex_set_key_actuation;
    bool ok = (g4b_act_key_set(s->key, (uint8_t)s->tenths) == 0);

    return APEX_RESPONSE(apex_set_key_actuation, ok);
}

ZMK_RPC_SUBSYSTEM_HANDLER(apex, apex_get_heatmap, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(apex, apex_reset_heatmap, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(apex, apex_get_status, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(apex, apex_get_actuation, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(apex, apex_set_key_actuation, ZMK_STUDIO_RPC_HANDLER_UNSECURED);

#endif /* CONFIG_ZMK_STUDIO */
