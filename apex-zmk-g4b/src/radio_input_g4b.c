/* SPDX-License-Identifier: MIT */
#include "apex_radio_input.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zmk/hid_indicators.h>

static atomic_t host_leds;
static void leds_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    struct zmk_hid_led_report_body report = {.leds = atomic_get(&host_leds)};
    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_RADIO};
    zmk_hid_indicators_process_report(&report, endpoint);
#endif
}
static K_WORK_DEFINE(leds_work, leds_work_fn);
void apex_radio_update_leds(uint8_t leds)
{
    if (atomic_set(&host_leds, leds) != leds) k_work_submit(&leds_work);
}
