/* SPDX-License-Identifier: MIT
 *
 * `apex ...` UART shell commands - a debug/power-user frontend over the shell
 * CDC-ACM port. Thin wrappers over apex_control (the same API Studio uses).
 * Compiled only when CONFIG_APEX_G4B_SHELL.
 */
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/init.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_backend.h>
#include <stdlib.h>
#include <errno.h>

#include <hal/nrf_power.h>

#include "apex_control_g4b.h"
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
#include "twi_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
#include "mode_g4b.h"
#include "actuation_g4b.h" /* link stats + per-key depth */
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_COREDUMP)
#include "coredump_g4b.h"
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
#include "rgb_g4b.h"
#endif
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_USB) || IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#endif

#define APEX_REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* Active HID output transport (what key reports actually go over right now). */
static const char *apex_active_transport(void)
{
#if IS_ENABLED(CONFIG_ZMK_USB) || IS_ENABLED(CONFIG_ZMK_BLE)
    switch (zmk_endpoint_get_selected().transport) {
    case ZMK_TRANSPORT_USB:
        return "USB";
    case ZMK_TRANSPORT_BLE:
        return "BLE";
    default:
        return "none";
    }
#else
    return "n/a";
#endif
}

/* Parse a millimetre string ("1.5", "2", "2.1") into tenths of a mm. */
static bool parse_mm_tenths(const char *s, uint8_t *out)
{
    char *end = NULL;
    long whole = strtol(s, &end, 10);
    if (whole < 0 || whole > 25) {
        return false;
    }
    long frac = 0;
    if (end && *end == '.') {
        char *fend = NULL;
        frac = strtol(end + 1, &fend, 10);
        if (frac < 0 || frac > 9) {
            return false;
        }
    }
    long tenths = whole * 10 + frac;
    if (tenths < 1 || tenths > 255) {
        return false;
    }
    *out = (uint8_t)tenths;
    return true;
}

static int cmd_battery(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct apex_battery b;
    if (!apex_battery_read(&b)) {
        shell_error(sh, "battery read failed (I2C)");
        return -EIO;
    }
    shell_print(sh, "battery: %u mV  %u%%  %u mA  %s%s  pg=%d",
                b.millivolts, b.percent, b.charge_ma,
                b.charging ? "charging" : "idle",
                b.terminated ? " (full)" : "", b.power_good);
    shell_print(sh, "  status=0x%02x fault=0x%02x", b.status_reg, b.fault_reg);
    return 0;
}

static int cmd_charge(const struct shell *sh, size_t argc, char **argv)
{
    if (argc >= 2) {
        const char *sub = argv[1];
        if (!strcmp(sub, "limit") && argc >= 3) {
            long pct = strtol(argv[2], NULL, 10);
            enum apex_charge_preset p = (pct >= 100) ? APEX_CHARGE_100 : APEX_CHARGE_80;
            if (!apex_charge_set_preset(p)) {
                shell_error(sh, "set preset failed");
                return -EIO;
            }
            shell_print(sh, "charge limit -> %d%%", (int)p);
        } else if (!strcmp(sub, "vreg") && argc >= 3) {
            long mv = strtol(argv[2], NULL, 10);
            if (!apex_charge_set_vreg_mv((uint16_t)mv)) {
                shell_error(sh, "set vreg failed");
                return -EIO;
            }
            shell_print(sh, "vreg -> %u mV (clamped to <= %u)",
                        (unsigned)mv, APEX_VREG_MAX_MV);
        } else if (!strcmp(sub, "current") && argc >= 3) {
            long ma = strtol(argv[2], NULL, 10);
            if (!apex_charge_set_current_ma((uint16_t)ma)) {
                shell_error(sh, "set current failed");
                return -EIO;
            }
            shell_print(sh, "charge current -> %u mA", (unsigned)ma);
        } else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
            bool on = !strcmp(sub, "on");
            if (!apex_charge_set_enabled(on)) {
                shell_error(sh, "set charging failed");
                return -EIO;
            }
            shell_print(sh, "charging %s", on ? "enabled" : "disabled (passthrough)");
        } else {
            shell_error(sh, "usage: apex charge [limit 80|100 | vreg <mV> | current <mA> | on | off]");
            return -EINVAL;
        }
    }
    struct apex_charge_cfg c;
    apex_charge_get(&c);
    shell_print(sh, "charge: vreg=%u mV  ichg=%u mA  %s  storage stop/resume=%u/%u%%",
                c.vreg_mv, c.ichg_ma,
                c.charging_enabled ? "enabled" : "passthrough",
                c.stop_pct, c.resume_pct);
    return 0;
}

static int cmd_act(const struct shell *sh, size_t argc, char **argv)
{
    if (argc >= 2) {
        uint8_t t;
        if (!parse_mm_tenths(argv[1], &t)) {
            shell_error(sh, "usage: apex act <mm>  (e.g. 1.5)");
            return -EINVAL;
        }
        apex_actuation_set_tenths(t);
    }
    uint8_t cur = apex_actuation_get_tenths();
    shell_print(sh, "actuation: %u.%u mm", cur / 10u, cur % 10u);
    return 0;
}

static int cmd_rt(const struct shell *sh, size_t argc, char **argv)
{
    if (argc >= 2) {
        if (!strcmp(argv[1], "off")) {
            apex_rapid_trigger_set(false, 0);
        } else if (!strcmp(argv[1], "on")) {
            uint8_t t = apex_rapid_trigger_get_tenths();
            if (argc >= 3 && !parse_mm_tenths(argv[2], &t)) {
                shell_error(sh, "usage: apex rt on <mm>");
                return -EINVAL;
            }
            apex_rapid_trigger_set(true, t ? t : 3);
        } else {
            shell_error(sh, "usage: apex rt [on <mm> | off]");
            return -EINVAL;
        }
    }
    if (apex_rapid_trigger_enabled()) {
        uint8_t t = apex_rapid_trigger_get_tenths();
        shell_print(sh, "rapid trigger: on, %u.%u mm", t / 10u, t % 10u);
    } else {
        shell_print(sh, "rapid trigger: off");
    }
    return 0;
}

static int cmd_rgb(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t count = apex_rgb_effect_count();
    if (count == 0u) {
        shell_print(sh, "rgb: not built in");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "bright")) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
        int dir = (argc >= 3 && !strcmp(argv[2], "down")) ? -1 : 1;
        (void)zmk_rgb_underglow_change_brt(dir);
        shell_print(sh, "brightness stepped %s", (dir > 0) ? "up" : "down");
#else
        shell_error(sh, "underglow not built in");
#endif
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "color")) {
        shell_print(sh, "per-key color from the shell is unsafe (SPIM2 is single-");
        shell_print(sh, "writer, owned by the RGB thread) - needs a static-light");
        shell_print(sh, "mode + RGB-thread hand-off. Not wired yet; use `apex rgb");
        shell_print(sh, "<effect>` for the built-in effects.");
        return 0;
    }
    if (argc >= 2) {
        long idx = strtol(argv[1], NULL, 10);
        if (idx < 0 || idx >= count || !apex_rgb_effect_set((uint8_t)idx)) {
            shell_error(sh, "usage: apex rgb [<0..%u> | bright up|down]", count - 1);
            return -EINVAL;
        }
    }
    uint8_t cur = apex_rgb_effect_get();
    shell_print(sh, "rgb effect: %u (%s)", cur, apex_rgb_effect_name(cur));
    for (uint8_t i = 0; i < count; i++) {
        shell_print(sh, "  %u: %s%s", i, apex_rgb_effect_name(i),
                    i == cur ? "  <-" : "");
    }
    return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct apex_telemetry t;
    apex_telemetry_read(&t);
    if (t.die_temp_valid) {
        int32_t mc = t.die_temp_mc;
        int whole = mc / 1000;
        int frac = (mc % 1000) / 100;
        if (frac < 0) {
            frac = -frac;
        }
        shell_print(sh, "SoC die temp : %d.%d C", whole, frac);
    } else {
        shell_print(sh, "SoC die temp : unavailable");
    }
    if (t.batt_temp_valid) {
        shell_print(sh, "battery temp : %d C  (BQ TS-pin NTC)", t.batt_temp_c);
    } else {
        shell_print(sh, "battery temp : unavailable");
    }
    shell_print(sh, "  (BQ TS raw REG10=%u, therm_reg=%d)",
                t.bq_ts_pct, t.bq_therm_regulating);
    return 0;
}

static int cmd_telemetry(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct apex_telemetry t;
    apex_telemetry_read(&t);
    shell_print(sh, "uptime: %u s  usb_conn=%u", t.uptime_s, t.usb_conn_state);
    if (t.die_temp_valid) {
        shell_print(sh, "die temp: %d mC", t.die_temp_mc);
    }
    shell_print(sh, "bq_ts=%u therm_reg=%d", t.bq_ts_pct, t.bq_therm_regulating);
    return 0;
}

/* ----------------------------------------------------- debug / SWD-replacement */

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(sh, "entering APEXBOOT DFU (GPREGRET=0x57, reset)...");
    shell_print(sh, "drop a .uf2 on the APEXBOOT drive to reflash.");
    nrf_power_gpregret_set(NRF_POWER, 0, 0x57u); /* DFU_MAGIC_UF2_RESET */
    k_msleep(60);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
    bool warm = (argc >= 2 && !strcmp(argv[1], "warm"));
    shell_print(sh, "rebooting (%s)...", warm ? "warm" : "cold");
    k_msleep(60);
    sys_reboot(warm ? SYS_REBOOT_WARM : SYS_REBOOT_COLD);
    return 0;
}

static int cmd_reginfo(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(sh, "RESETREAS = 0x%08x", APEX_REG32(0x40000400));
    shell_print(sh, "GPREGRET  = 0x%02x   GPREGRET2 = 0x%02x",
                APEX_REG32(0x4000051C) & 0xffu, APEX_REG32(0x40000520) & 0xffu);
    shell_print(sh, "SCB VTOR  = 0x%08x", APEX_REG32(0xE000ED08));
    shell_print(sh, "UICR boot = 0x%08x   mbrparam = 0x%08x",
                APEX_REG32(0x10001014), APEX_REG32(0x10001018));
    shell_print(sh, "UICR REGOUT0 = 0x%08x   APPROTECT = 0x%08x",
                APEX_REG32(0x10001304), APEX_REG32(0x10001208));
    shell_print(sh, "FICR DEVICEID = %08x%08x   PART = 0x%08x  VARIANT = 0x%08x",
                APEX_REG32(0x10000064), APEX_REG32(0x10000060),
                APEX_REG32(0x10000100), APEX_REG32(0x10000104));
    return 0;
}

static int cmd_uicr(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    for (uint32_t off = 0; off < 0x100u; off += 16u) {
        uint32_t a = 0x10001000u + off;
        shell_print(sh, "0x%08x: %08x %08x %08x %08x", a,
                    APEX_REG32(a), APEX_REG32(a + 4u),
                    APEX_REG32(a + 8u), APEX_REG32(a + 12u));
    }
    return 0;
}

#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
static int cmd_bqreg(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: apex bqreg <reg 0x00..0x14>");
        return -EINVAL;
    }
    long reg = strtol(argv[1], NULL, 0);
    if (reg < 0 || reg > 0x14) {
        shell_error(sh, "reg out of range (0x00..0x14)");
        return -EINVAL;
    }
    struct g4b_twi_result r = g4b_bq_read((uint8_t)reg);
    if (!r.ok) {
        shell_error(sh, "BQ read failed (errorsrc=0x%02x)", r.errorsrc);
        return -EIO;
    }
    shell_print(sh, "BQ REG%02x = 0x%02x", (unsigned)reg, r.value);
    return 0;
}

static int cmd_bqdump(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    for (uint8_t r = 0; r <= 0x14u; r++) {
        struct g4b_twi_result res = g4b_bq_read(r);
        if (res.ok) {
            shell_print(sh, "REG%02x = 0x%02x", r, res.value);
        } else {
            shell_print(sh, "REG%02x = <read failed>", r);
        }
    }
    return 0;
}
#endif /* CONFIG_APEX_G4B_TWI */

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    uint32_t kp = apex_stats_keypresses();
    uint32_t up = (uint32_t)(k_uptime_get() / 1000);
    shell_print(sh, "key presses : %u", kp);
    shell_print(sh, "uptime      : %u s", up);
    if (up) {
        shell_print(sh, "average     : %u presses/min", (kp * 60u) / up);
    }
    return 0;
}

#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
    if (argc >= 2) {
        int m = -2;
        if (!strcmp(argv[1], "bt") || !strcmp(argv[1], "bluetooth")) {
            m = G4B_MODE_BT;
        } else if (!strcmp(argv[1], "usb")) {
            m = G4B_MODE_USB;
        } else if (!strcmp(argv[1], "dongle") || !strcmp(argv[1], "24")) {
            m = G4B_MODE_DONGLE;
        } else if (!strcmp(argv[1], "auto")) {
            m = -1;
        }
        if (m == -2) {
            shell_error(sh, "usage: apex mode [bt|usb|dongle|auto]");
            return -EINVAL;
        }
        apex_mode_set(m); /* persists to NVS */
        shell_print(sh, "mode override -> %s (saved)",
                    (m < 0) ? "auto (follow switch)" : argv[1]);
        if (m == G4B_MODE_DONGLE) {
            shell_print(sh, "(dongle = 2.4 GHz radio owner; in radio builds this "
                            "reboots to switch stacks. Override persists the reset.)");
        }
    }
    enum g4b_mode cur = g4b_mode_get();
    const char *name = (cur == G4B_MODE_BT) ? "Bluetooth"
                     : (cur == G4B_MODE_USB) ? "USB"
                     : (cur == G4B_MODE_DONGLE) ? "2.4 GHz dongle" : "unknown";
    int ov = g4b_mode_get_override();
    shell_print(sh, "mode: %s   switch reads %u mV   source: %s", name,
                g4b_mode_mv(), (ov >= 0) ? "FORCED (override)" : "physical switch");
    shell_print(sh, "active HID transport: %s (a ready USB host wins in every mode)",
                apex_active_transport());
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
static int cmd_ble(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    int idx = zmk_ble_active_profile_index();
    shell_print(sh, "active profile: %d (%s)", idx, zmk_ble_active_profile_name());
    shell_print(sh, "  connected=%d  open=%d",
                zmk_ble_active_profile_is_connected(),
                zmk_ble_active_profile_is_open());
    for (uint8_t i = 0; i < 5; i++) {
        shell_print(sh, "  profile %u: %s%s", i,
                    zmk_ble_profile_is_connected(i) ? "connected"
                        : (zmk_ble_profile_is_open(i) ? "open" : "bonded/idle"),
                    (i == idx) ? "  <- active" : "");
    }
    return 0;
}
#endif

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct apex_battery b;
    struct apex_charge_cfg c;
    struct apex_telemetry t;
    bool bok = apex_battery_read(&b);
    apex_charge_get(&c);
    apex_telemetry_read(&t);
    uint8_t act = apex_actuation_get_tenths();
    uint8_t rgb = apex_rgb_effect_get();

    shell_print(sh, "== Apex Pro Mini WL ==");
    shell_print(sh, "uptime    : %u s   usb_conn=%u   hid=%s   keypresses=%u",
                t.uptime_s, t.usb_conn_state, apex_active_transport(), t.keypresses);
    if (bok) {
        shell_print(sh, "battery   : %u mV  %u%%  %u mA  %s%s  pg=%d",
                    b.millivolts, b.percent, b.charge_ma,
                    b.charging ? "charging" : "idle",
                    b.terminated ? " (full)" : "", b.power_good);
    }
    shell_print(sh, "charge    : vreg=%u mV  ichg=%u mA  %s  stop/resume=%u/%u%%",
                c.vreg_mv, c.ichg_ma, c.charging_enabled ? "on" : "passthrough",
                c.stop_pct, c.resume_pct);
    shell_print(sh, "actuation : %u.%u mm   rapid trigger: %s",
                act / 10u, act % 10u, apex_rapid_trigger_enabled() ? "on" : "off");
    shell_print(sh, "rgb       : effect %u (%s) of %u", rgb,
                apex_rgb_effect_name(rgb), apex_rgb_effect_count());
    if (t.die_temp_valid) {
        int w = t.die_temp_mc / 1000;
        int f = (t.die_temp_mc % 1000) / 100;
        if (f < 0) {
            f = -f;
        }
        shell_print(sh, "die temp  : %d.%d C", w, f);
    }
    if (t.batt_temp_valid) {
        shell_print(sh, "batt temp : %d C", t.batt_temp_c);
    }
    return 0;
}

static int cmd_mon(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t count = 20u;
    uint32_t interval_ms = 1000u;
    if (argc >= 2) {
        count = strtoul(argv[1], NULL, 10);
    }
    if (argc >= 3) {
        interval_ms = strtoul(argv[2], NULL, 10) * 1000u;
    }
    if (count == 0u || count > 600u) {
        count = 20u;
    }
    if (interval_ms < 200u) {
        interval_ms = 1000u;
    }
    shell_print(sh, "monitoring %u samples @ %u ms (blocks the shell until done)...",
                count, interval_ms);
    for (uint32_t i = 0; i < count; i++) {
        struct apex_battery b;
        struct apex_telemetry t;
        bool bok = apex_battery_read(&b);
        apex_telemetry_read(&t);
        int tw = t.die_temp_valid ? (t.die_temp_mc / 1000) : 0;
        shell_print(sh, "[%3u] %5u mV  %3u%%  %-4s  die %2d C  keys %u  hid %s",
                    i, bok ? b.millivolts : 0u, bok ? b.percent : 0u,
                    (bok && b.charging) ? "chg" : "idle", tw, t.keypresses,
                    apex_active_transport());
        k_sleep(K_MSEC(interval_ms));
    }
    return 0;
}

#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
static int cmd_link(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct g4b_link_stats s;
    g4b_link_stats_get(&s);
    uint32_t mpct = s.frames_run ? (s.frames_matched * 100u / s.frames_run) : 0u;
    shell_print(sh, "STM32 scanner link health:");
    /* Boot handshake: one-shot, proves the link came up. Frozen after handover
     * to the keyboard loop - do NOT read these as live activity. */
    shell_print(sh, "  boot handshake : %u/%u frames matched (%u%%)",
                s.frames_matched, s.frames_run, mpct);
    /* Live: these climb while the keyboard loop runs. key events rises each time
     * you press or release a key; keepalives tick on idle 0xA0 polls. */
    shell_print(sh, "  key events     : %u (live)", s.live_key_events);
    shell_print(sh, "  idle keepalives: %u (live)", s.live_keepalives);
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_RGB) && IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
static int cmd_depth(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    uint16_t d[72];
    /* Ask the link thread to sample 0xA2 (it owns the SPIM bus); give it a few
     * window sweeps, then read the smoothed per-key travel. */
    g4b_depth_force(true);
    k_sleep(K_MSEC(400));
    size_t n = g4b_depth_read(d, ARRAY_SIZE(d));
    g4b_depth_force(false);
    if (n > ARRAY_SIZE(d)) {
        n = ARRAY_SIZE(d);
    }
    shell_print(sh, "per-key Hall travel (pressed keys only; 0%%=released):");
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (d[i] == 0u) {
            continue; /* not sampled */
        }
        uint32_t pct = (d[i] <= 337u) ? 0u
                     : (d[i] >= 3959u) ? 100u
                     : ((uint32_t)(d[i] - 337u) * 100u) / (3959u - 337u);
        if (pct >= 5u) {
            shell_print(sh, "  key %2u: %3u%%  (raw %u)", (unsigned)i, pct, d[i]);
            any = true;
        }
    }
    if (!any) {
        shell_print(sh, "  (nothing pressed - hold a key while running this)");
    }
    return 0;
}

/* Full raw-Hall sweep of all 70 scan slots, unfiltered - for measuring crosstalk
 * coupling (0x36 topology). Uses the same in-loop 0xA2 sampling as `apex depth`
 * (no desync). Read once at rest and once with a key held; the slots whose raw
 * value shifts are that key's electromagnetic neighbours. */
static int cmd_halldump(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    uint16_t d[72];
    g4b_depth_force(true);
    k_sleep(K_MSEC(400));
    size_t n = g4b_depth_read(d, ARRAY_SIZE(d));
    g4b_depth_force(false);
    if (n > 70u) {
        n = 70u;
    }
    for (size_t i = 0; i < n; i += 10u) {
        char line[100];
        size_t p = 0u;
        for (size_t j = i; j < i + 10u && j < n; j++) {
            p += snprintk(&line[p], sizeof(line) - p, "%4u ", d[j]);
        }
        shell_print(sh, "H[%2u]: %s", (unsigned)i, line);
    }
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
/* Raw STM32 scanner frame probe - the debug lever for the link protocol. Send an
 * opcode (+ optional args) as hex; print the 64-byte reply. Examples:
 *   apex scanraw 90        version query -> ASCII "3.24.1"
 *   apex scanraw a1        key bitmap (9 bytes)
 *   apex scanraw 20        query scanner state
 *   apex scanraw a2 08 00  read 8 per-key Hall samples from index 0
 * The exchange runs on the g4b thread (single-writer safe). Opcodes 0x01/0x02/
 * 0x32 are refused (scanner reset / power poke / flash write - see PROTOCOL.md). */
static int cmd_scanraw(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t tx[64] = {0};
    uint32_t n = 0u;

    if (argc < 2) {
        shell_print(sh, "usage: apex scanraw <hex byte> [hex byte...]  (e.g. 90, a1, 20, a2 08 00)");
        return 0;
    }
    for (size_t a = 1u; a < argc && n < sizeof(tx); a++) {
        const char *s = argv[a];
        /* each arg is one or more hex byte(s): "90" or "9000" */
        for (size_t i = 0u; s[i] != '\0' && s[i + 1] != '\0' && n < sizeof(tx); i += 2u) {
            char pair[3] = { s[i], s[i + 1], '\0' };
            char *end = NULL;
            unsigned long v = strtoul(pair, &end, 16);
            if (end == pair) {
                shell_print(sh, "bad hex near '%s'", s);
                return 0;
            }
            tx[n++] = (uint8_t)v;
        }
    }
    if (n == 0u) {
        shell_print(sh, "no bytes parsed");
        return 0;
    }
    if (tx[0] == 0x01u || tx[0] == 0x02u || tx[0] == 0x32u) {
        shell_print(sh, "refused: opcode 0x%02x is destructive "
                        "(0x01 reset / 0x02 power / 0x32 flash write)", tx[0]);
        return 0;
    }

    uint8_t rx[64];
    int rc = g4b_scan_raw(tx, n, rx, 500u);
    if (rc != 0) {
        shell_print(sh, "exchange failed (rc=%d)", rc);
        return 0;
    }

    char line[3 * 24 + 1];
    size_t p = 0u;
    for (uint32_t i = 0u; i < 24u && p < sizeof(line) - 3u; i++) {
        p += snprintk(&line[p], sizeof(line) - p, "%02x ", rx[i]);
    }
    shell_print(sh, "tx 0x%02x (%u B) -> rx[0..23]: %s", tx[0], (unsigned)n, line);
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_APEX_G4B_COREDUMP)
static const char *fatal_reason_name(uint32_t r)
{
    switch (r) {
    case 0: return "CPU exception";
    case 1: return "spurious IRQ";
    case 2: return "stack overflow";
    case 3: return "kernel oops";
    case 4: return "kernel panic";
    default: return "unknown";
    }
}

static int cmd_crash(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct g4b_coredump_record r;
    if (!g4b_coredump_read_last(&r)) {
        shell_print(sh, "no crash recorded (clean history)");
        return 0;
    }
    shell_print(sh, "last crash (record #%u):", r.seq);
    shell_print(sh, "  reason   : %u (%s)", r.reason, fatal_reason_name(r.reason));
    shell_print(sh, "  PC       : 0x%08x    LR : 0x%08x", r.pc, r.lr);
    shell_print(sh, "  xPSR     : 0x%08x    SP : 0x%08x", r.xpsr, r.sp);
    shell_print(sh, "  CFSR     : 0x%08x    HFSR: 0x%08x", r.cfsr, r.hfsr);
    if (r.cfsr & (1u << 25)) {
        shell_print(sh, "    - divide by zero");
    }
    if (r.cfsr & (1u << 24)) {
        shell_print(sh, "    - unaligned access");
    }
    if (r.cfsr & (1u << 16)) {
        shell_print(sh, "    - undefined instruction");
    }
    if (r.cfsr & (1u << 1)) {
        shell_print(sh, "    - precise data bus fault @ 0x%08x", r.bfar);
    }
    if (r.cfsr & (1u << 7)) {
        shell_print(sh, "    - mem-manage fault @ 0x%08x", r.mmfar);
    }
    shell_print(sh, "  RESETREAS: 0x%08x", r.resetreas);
    return 0;
}
#endif

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "usage: apex reset <nrf|dfu|stm32|usb|rgb|charger|ble>");
        shell_print(sh, "  nrf     - reboot the whole keyboard (cold)");
        shell_print(sh, "  dfu     - reboot into the APEXBOOT bootloader");
        shell_print(sh, "  stm32   - pulse the scanner enable lines (reboots the scanner)");
        shell_print(sh, "  usb     - pulse the USB rail (re-enumerate; always restores)");
        shell_print(sh, "  rgb     - power-cycle + re-init the LED controller");
        shell_print(sh, "  charger - re-apply the safe BQ25895 config (4.096 V)");
        shell_print(sh, "  ble     - drop the active Bluetooth connection");
        return 0;
    }
    const char *t = argv[1];
    if (!strcmp(t, "nrf") || !strcmp(t, "board")) {
        shell_print(sh, "rebooting...");
        k_msleep(50);
        sys_reboot(SYS_REBOOT_COLD);
    } else if (!strcmp(t, "dfu")) {
        return cmd_dfu(sh, argc, argv);
    }
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    else if (!strcmp(t, "stm32") || !strcmp(t, "scanner")) {
        g4b_request_stm32_reset();
        shell_print(sh, "STM32 scanner reset requested (enable-line pulse at next idle).");
        shell_print(sh, "if the keyboard stops typing afterwards, run `apex reboot`.");
    } else if (!strcmp(t, "usb")) {
        shell_print(sh, "pulsing the USB rail (~150 ms) - this shell will drop and");
        shell_print(sh, "reconnect. The board always brings USB back up.");
        g4b_request_usb_rail_reset();
    }
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    else if (!strcmp(t, "rgb") || !strcmp(t, "leds")) {
        g4b_request_rgb_reset();
        shell_print(sh, "RGB controller reset requested (rail cycle + re-init).");
    }
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    else if (!strcmp(t, "charger") || !strcmp(t, "bq")) {
        bool ok = g4b_bq_configure_charge();
        shell_print(sh, "charger re-configured to safe defaults (4.096 V): %s",
                    ok ? "ok" : "FAILED");
    }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
    else if (!strcmp(t, "ble")) {
        struct bt_conn *c = zmk_ble_active_profile_conn();
        if (c != NULL) {
            (void)bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            bt_conn_unref(c);
            shell_print(sh, "dropped the active BLE connection");
        } else {
            shell_print(sh, "no active BLE connection");
        }
    }
#endif
    else {
        shell_error(sh, "unknown target '%s'", t);
        return -EINVAL;
    }
    return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_print(sh, "usage: apex power <rgb|charge> <on|off>");
        shell_print(sh, "  rgb    - LED rail power (safe to leave off)");
        shell_print(sh, "  charge - battery charging (passthrough when off)");
        shell_print(sh, "  (USB power is intentionally not exposed - can't drop it safely)");
        return 0;
    }
    bool on;
    if (!strcmp(argv[2], "on")) {
        on = true;
    } else if (!strcmp(argv[2], "off")) {
        on = false;
    } else {
        shell_error(sh, "expected on|off");
        return -EINVAL;
    }
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    if (!strcmp(argv[1], "rgb") || !strcmp(argv[1], "leds")) {
        g4b_request_rgb_rail(on);
        shell_print(sh, "RGB rail power %s requested", on ? "ON" : "OFF");
        return 0;
    }
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    if (!strcmp(argv[1], "charge") || !strcmp(argv[1], "charging")) {
        bool ok = apex_charge_set_enabled(on);
        shell_print(sh, "charging %s: %s", on ? "enabled" : "disabled (passthrough)",
                    ok ? "ok" : "FAILED");
        return 0;
    }
#endif
    shell_error(sh, "unknown rail '%s' (try: rgb, charge)", argv[1]);
    return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    apex_sub,
    SHELL_CMD(info, NULL,
              "One-shot dashboard: battery, charge, actuation, RT, RGB, temps, "
              "uptime, keypresses, active HID transport.\nUsage: apex info",
              cmd_info),
    SHELL_CMD(mon, NULL,
              "Live monitor: a compact status line each interval.\n"
              "Usage: apex mon [count] [seconds]   (default 20 @ 1 s; "
              "blocks the shell until it finishes).",
              cmd_mon),
    SHELL_CMD(battery, NULL,
              "Battery status.\n"
              "Usage: apex battery\n"
              "Shows mV, %, charge mA, charging/idle/full, power-good, "
              "and raw BQ status/fault registers.",
              cmd_battery),
    SHELL_CMD(charge, NULL,
              "Charger (BQ25895) config.\n"
              "Usage: apex charge [limit 80|100 | vreg <mV> | current <mA> | on | off]\n"
              "No args prints the current config. vreg is hard-clamped <=4400 mV "
              "(pack rated max). limit: 80=4096mV, 100=4352mV.",
              cmd_charge),
    SHELL_CMD(act, NULL,
              "Actuation point (global).\n"
              "Usage: apex act [<mm>]   e.g. apex act 1.5\n"
              "No args prints current. Snaps to the switch ladder (1.0-3.0 mm).",
              cmd_act),
    SHELL_CMD(rt, NULL,
              "Rapid trigger.\n"
              "Usage: apex rt [on <mm> | off]   e.g. apex rt on 0.3\n"
              "No args prints current state.",
              cmd_rt),
    SHELL_CMD(rgb, NULL,
              "RGB effects + brightness.\n"
              "Usage: apex rgb [<index 0..11> | bright up|down]\n"
              "No args lists the 12 effects and marks the active one.",
              cmd_rgb),
    SHELL_CMD(temp, NULL,
              "Temperatures.\n"
              "Usage: apex temp\n"
              "SoC die temp (nRF TEMP) and BQ TS pin.",
              cmd_temp),
    SHELL_CMD(telemetry, NULL,
              "Live telemetry snapshot.\n"
              "Usage: apex telemetry\n"
              "Uptime, USB connection state, temps.",
              cmd_telemetry),
    SHELL_CMD(stats, NULL,
              "Usage statistics.\n"
              "Usage: apex stats\n"
              "Total key presses since boot, uptime, average presses/min.",
              cmd_stats),
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    SHELL_CMD(mode, NULL,
              "Get/force the transport mode, regardless of the physical switch.\n"
              "Usage: apex mode [bt | usb | dongle | auto]\n"
              "No args shows current mode + whether it's forced. `auto` returns to "
              "following the switch. USB<->BT apply instantly; dongle owns the "
              "2.4 GHz radio (radio builds reboot to switch stacks; the override "
              "persists that reset).",
              cmd_mode),
    SHELL_CMD(link, NULL,
              "STM32 scanner link health (frames run/matched, ingest ok).\n"
              "Usage: apex link",
              cmd_link),
#if IS_ENABLED(CONFIG_APEX_G4B_RGB)
    SHELL_CMD(depth, NULL,
              "Live per-key Hall travel (%%), for tuning actuation.\n"
              "Usage: apex depth   (hold the keys you want to read while running)",
              cmd_depth),
    SHELL_CMD(halldump, NULL,
              "Raw Hall value of all 70 scan slots (unfiltered), for crosstalk RE.\n"
              "Usage: apex halldump   (read at rest, then with a key held; diff)",
              cmd_halldump),
#endif
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_SPIM_KSCAN)
    SHELL_CMD(scanraw, NULL,
              "Send a raw frame to the STM32 scanner and print the reply (debug).\n"
              "Usage: apex scanraw <hex> [hex...]   e.g. 90 (version), a1 (keys), 20 (state)",
              cmd_scanraw),
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
    SHELL_CMD(ble, NULL,
              "BLE profile status (active index, connected/open, per-profile).\n"
              "Usage: apex ble",
              cmd_ble),
#endif
    SHELL_CMD(dfu, NULL,
              "Reboot into the APEXBOOT USB bootloader (DFU) - no button/SWD needed.\n"
              "Usage: apex dfu\n"
              "Writes GPREGRET=0x57 and resets; then drop a .uf2 on the APEXBOOT drive.",
              cmd_dfu),
    SHELL_CMD(reboot, NULL,
              "Reboot the keyboard.\n"
              "Usage: apex reboot [warm|cold]   (default cold)",
              cmd_reboot),
    SHELL_CMD(reset, NULL,
              "Reset a component (nRF, scanner, USB rail, LEDs, charger, BLE).\n"
              "Usage: apex reset <nrf|dfu|stm32|usb|rgb|charger|ble>\n"
              "No args lists targets. The USB rail is only ever pulsed (always "
              "restored), never left off.",
              cmd_reset),
    SHELL_CMD(power, NULL,
              "Direct rail power control.\n"
              "Usage: apex power <rgb|charge> <on|off>\n"
              "LED rail may stay off; charging off = passthrough. USB not exposed.",
              cmd_power),
    SHELL_CMD(reginfo, NULL,
              "Dump key CPU/nRF registers (SWD-style).\n"
              "Usage: apex reginfo\n"
              "RESETREAS, GPREGRET, SCB VTOR, UICR boot/mbr/REGOUT0/APPROTECT, "
              "FICR device id/part/variant.",
              cmd_reginfo),
    SHELL_CMD(uicr, NULL,
              "Hexdump the UICR customer region (0x10001000, 256 bytes).\n"
              "Usage: apex uicr",
              cmd_uicr),
#if IS_ENABLED(CONFIG_APEX_G4B_COREDUMP)
    SHELL_CMD(crash, NULL,
              "Show the last stored crash (reason, PC/LR, fault status, decoded).\n"
              "Usage: apex crash",
              cmd_crash),
#endif
#if IS_ENABLED(CONFIG_APEX_G4B_TWI)
    SHELL_CMD(bqreg, NULL,
              "Read a BQ25895 register (read-only).\n"
              "Usage: apex bqreg <reg 0x00..0x14>",
              cmd_bqreg),
    SHELL_CMD(bqdump, NULL,
              "Dump all BQ25895 registers (0x00..0x14, read-only).\n"
              "Usage: apex bqdump",
              cmd_bqdump),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(apex, &apex_sub,
                   "Apex Pro Mini WL control + debug console. "
                   "Try `apex`, or `apex <cmd> -h` for usage. "
                   "General tools: devmem, flash, hwinfo, kernel, log, help.",
                   NULL);

/* Quiet the log flood so the interactive shell is usable by default. Logs stay
 * COMPILED at their full level (ZMK is DBG), so the runtime level can be raised
 * again on demand: `log enable dbg zmk`, `log enable inf`, or `log go`/`log halt`.
 * Runs from a delayed work item ~300 ms after boot, after every backend
 * (including the shell log backend) has registered, so the filter sticks. */
static void apex_quiet_logs_work(struct k_work *work)
{
    ARG_UNUSED(work);
    uint32_t sources = log_src_cnt_get(0);
    int backends = log_backend_count_get();
    for (int b = 0; b < backends; b++) {
        const struct log_backend *be = log_backend_get(b);
        for (uint32_t s = 0; s < sources; s++) {
            (void)log_filter_set(be, 0, (int16_t)s, LOG_LEVEL_ERR);
        }
    }
}
static K_WORK_DELAYABLE_DEFINE(apex_quiet_logs_dwork, apex_quiet_logs_work);

static int apex_quiet_logs_init(void)
{
    (void)k_work_schedule(&apex_quiet_logs_dwork, K_MSEC(300));
    return 0;
}
SYS_INIT(apex_quiet_logs_init, APPLICATION, 99);
