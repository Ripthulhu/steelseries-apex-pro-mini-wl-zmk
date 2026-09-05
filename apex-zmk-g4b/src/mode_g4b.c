/* SPDX-License-Identifier: MIT
 *
 * Mode-switch transport policy:
 *   - Bluetooth: BLE is available and a ready USB HID host takes priority.
 *   - USB: BLE is disabled and reports use USB.
 *   - Dongle: release builds use the USB-only policy because the 2.4 GHz
 *     transport is unfinished. Experimental radio builds reserve NRF_RADIO and
 *     use their own scanner-health policy in this position.
 *
 * The switch is sampled once per second by the watchdog thread.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include "mode_g4b.h"
#include "saadc_g4b.h"

#if IS_ENABLED(CONFIG_APEX_G4B_DONGLE_RADIO)
#include <cmsis_core.h>
#include "radio_g4b.h"
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <hal/nrf_power.h>
#include <zmk/usb.h>
#endif

#define G4B_MODE_PSELP         2u   /* AIN1 -> P0.03 */
#define G4B_MODE_SAMPLES       8u
#define G4B_MODE_USB_MIN_MV    900u
#define G4B_MODE_DONGLE_MIN_MV 2600u

/* Consecutive one-second readings required before committing a mode change. */
#define G4B_MODE_DEBOUNCE      3u

static enum g4b_mode mode_cached = G4B_MODE_BT;
static enum g4b_mode mode_pending = G4B_MODE_BT;
static uint8_t mode_pending_count;
static bool mode_primed;
static uint16_t mode_mv_cached;
static uint32_t mode_last_sample_ms;

/* Sticky threshold-crossing flags used by mode diagnostics. */
static uint8_t mode_flags;

/* Boot-time position used to assign radio ownership. */
static bool boot_is_dongle;

/* Software override of the switch: -1 = follow the switch, else a forced
 * g4b_mode. Kept in NOINIT RAM so a forced dongle/non-dongle survives the warm
 * reset that reassigns the radio; the magic guards against power-on garbage. */
#define G4B_MODE_RETAIN_MAGIC 0x4D4F4445u /* "MODE" */
static __noinit struct {
    uint32_t magic;
    int8_t mode;
} mode_retain;
static int8_t mode_override = -1;

/* The thresholds lie between the measured switch levels of approximately
 * 0 mV, 1670 mV, and 3345 mV. Their smallest margin is about 700 mV, well above
 * the SAADC error budget. A failed SAADC read leaves the cached mode and sample
 * timestamp unchanged so the watchdog can detect a stalled sampler.
 */
static enum g4b_mode classify(uint16_t mv)
{
    if (mv >= G4B_MODE_DONGLE_MIN_MV) {
        return G4B_MODE_DONGLE;
    }
    if (mv >= G4B_MODE_USB_MIN_MV) {
        return G4B_MODE_USB;
    }

    return G4B_MODE_BT;
}

void g4b_mode_sample(void)
{
    uint32_t acc = 0U;

    for (uint32_t i = 0U; i < G4B_MODE_SAMPLES; i++) {
        int16_t raw = g4b_saadc_read(G4B_MODE_PSELP);

        if (raw == G4B_SAADC_READ_FAILED) {
            return;
        }
        if (raw > 0) {
            acc += (uint32_t)(uint16_t)raw;
        }
    }

    mode_mv_cached = (uint16_t)((acc / G4B_MODE_SAMPLES) * 3600U / 4096U);

    /* Debounce mode changes to prevent a transient sample from dropping BLE.
     * Accept the first sample immediately for correct boot-time radio ownership.
     */
    enum g4b_mode raw = classify(mode_mv_cached);

    if (!mode_primed) {
        mode_cached = raw;
        mode_primed = true;
    } else if (raw == mode_cached) {
        mode_pending_count = 0u;
    } else if (raw == mode_pending) {
        if (++mode_pending_count >= G4B_MODE_DEBOUNCE) {
            mode_cached = raw;
            mode_pending_count = 0u;
        }
    } else {
        mode_pending = raw;
        mode_pending_count = 1u;
    }

    /* Record threshold crossings for diagnostics without changing behaviour. */
    mode_flags |= (mode_mv_cached >= G4B_MODE_DONGLE_MIN_MV)
                      ? G4B_MODE_SEEN_HIGH
                      : G4B_MODE_SEEN_LOW;

    /* Update freshness only after a complete sample batch. */
    mode_last_sample_ms = k_uptime_get_32();
}

enum g4b_mode g4b_mode_get(void)
{
    if (mode_override >= 0) {
        return (enum g4b_mode)mode_override;
    }
    return mode_cached;
}

uint16_t g4b_mode_mv(void)
{
    return mode_mv_cached;
}

bool g4b_mode_boot_dongle(void)
{
    return boot_is_dongle;
}

uint8_t g4b_mode_flags(void)
{
    return mode_flags;
}

uint32_t g4b_mode_last_sample_ms(void)
{
    return mode_last_sample_ms;
}

static void mode_gate_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(mode_gate, mode_gate_work);

/* Require both VBUS and a ready HID endpoint before selecting USB. */
#if IS_ENABLED(CONFIG_ZMK_BLE)
static bool usb_host_ready(void)
{
#if IS_ENABLED(CONFIG_ZMK_USB)
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0u &&
           zmk_usb_is_hid_ready();
#else
    return false;
#endif
}
#endif

static void mode_gate_work(struct k_work *work)
{
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_APEX_G4B_DONGLE_RADIO)
    /* Reset after a settled dongle-boundary change to reassign radio ownership.
     * Uses g4b_mode_get() so a software override crosses the boundary too. */
    if ((g4b_mode_get() == G4B_MODE_DONGLE) != boot_is_dongle) {
        NVIC_SystemReset();
    }

    /* Skip Bluetooth API calls after the controller has been disabled. */
    if (g4b_radio_stood_down()) {
        k_work_schedule(&mode_gate, K_SECONDS(1));
        return;
    }
#endif

    /* Sampling runs in the watchdog thread; this work item applies the cached
     * transport policy from a context where Bluetooth APIs are available.
     */
#if IS_ENABLED(CONFIG_ZMK_BLE)
    /* Edge-triggered: re-select the report transport only when the desired value
     * changes (avoids a per-tick ZMK transport log and "Not sending" warning).
     *
     * Advertising is owned by ZMK's update_advertising() (ble.c). Do NOT call
     * bt_le_adv_stop()/bt_conn_disconnect() here: ZMK ignores the preferred
     * transport when deciding to advertise and re-runs on every disconnect, so a
     * stop behind its back races the controller LLL and asserts in lll_adv.c
     * prepare_cb (kernel oops on a mode switch). Setting the preferred transport
     * is enough; a live BLE link just stops being the report sink. */
    static int last_want = -1;

    enum zmk_transport want;
    if (mode_cached != G4B_MODE_BT) {
        want = ZMK_TRANSPORT_USB;
    } else {
        /* Prefer a ready USB host; keep BLE selected for charge-only sources. */
        want = usb_host_ready() ? ZMK_TRANSPORT_USB : ZMK_TRANSPORT_BLE;
    }

    if ((int)want != last_want) {
        (void)zmk_endpoint_set_preferred_transport(want);
        last_want = (int)want;
    }
#endif

    k_work_schedule(&mode_gate, K_SECONDS(1));
}

void g4b_mode_set_override(int mode)
{
    if (mode < G4B_MODE_BT || mode > G4B_MODE_DONGLE) {
        mode_override = -1;
        mode_retain.magic = 0u;
    } else {
        mode_override = (int8_t)mode;
        mode_retain.magic = G4B_MODE_RETAIN_MAGIC;
        mode_retain.mode = (int8_t)mode;
    }
    /* Apply the new transport policy now instead of waiting up to a second (and,
     * in radio builds, trigger the radio-owner reset from mode_gate_work). */
    k_work_reschedule(&mode_gate, K_NO_WAIT);
}

int g4b_mode_get_override(void)
{
    return mode_override;
}

static int g4b_mode_init(void)
{
    /* Sample before the watchdog module initializes at APPLICATION 98. */
    g4b_mode_sample();

    /* Restore a persisted override BEFORE latching radio ownership, so a forced
     * dongle/non-dongle survives the reset that reassigns the radio. */
    if (mode_retain.magic == G4B_MODE_RETAIN_MAGIC &&
        mode_retain.mode >= G4B_MODE_BT && mode_retain.mode <= G4B_MODE_DONGLE) {
        mode_override = mode_retain.mode;
    }

    /* Latch the boot position for radio ownership and change detection. */
    boot_is_dongle = (g4b_mode_get() == G4B_MODE_DONGLE);

    k_work_schedule(&mode_gate, K_SECONDS(1));

    return 0;
}

SYS_INIT(g4b_mode_init, APPLICATION, 90);
