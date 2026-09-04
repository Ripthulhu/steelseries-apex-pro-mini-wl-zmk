/* SPDX-License-Identifier: MIT
 *
 * Bounded single-shot SAADC reads. The three-position mode switch is a resistor
 * divider on P0.03/AIN1 with measured levels near 0 V, 1.6 V, and 3.3 V. The
 * remaining channel list supports development measurements without sampling
 * pins owned by the STM32 link. All peripheral waits are iteration-bounded.
 */

#include <zephyr/kernel.h>

#include <nrfx.h>

#include "saadc_g4b.h"

/* PSELP values, per the nRF52833 SAADC: 1..8 select AIN0..AIN7. */
const uint8_t g4b_saadc_pselp[G4B_SAADC_CHANNELS] = {
    2u, /* AIN1 -> P0.03 */
    5u, /* AIN4 -> P0.28 */
    6u, /* AIN5 -> P0.29 */
    7u, /* AIN6 -> P0.30 */
    8u, /* AIN7 -> P0.31 */
};

#define G4B_SAADC_SPIN 20000u

/* The SAADC has two callers: the stage thread's diagnostic sweep and the
 * watchdog thread's mode sample. There is a single CH[0] and a
 * single RESULT.PTR between them. Without this, one caller can retarget PSELP
 * while the other is mid-conversion and both get a value from the wrong pin,
 * potentially producing a false mode-switch reading.
 */
static K_MUTEX_DEFINE(saadc_lock);

static bool wait_event(volatile uint32_t *event)
{
    for (uint32_t i = 0U; i < G4B_SAADC_SPIN; i++) {
        if (*event != 0U) {
            *event = 0U;
            return true;
        }
    }

    return false;
}

int16_t g4b_saadc_read(uint8_t pselp)
{
    static volatile int16_t result;
    bool completed = false;

    k_mutex_lock(&saadc_lock, K_FOREVER);

    result = G4B_SAADC_READ_FAILED;

    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;

    /* Single-ended, gain 1/6 against the internal 0.6 V reference, so full
     * scale is 3.6 V and a 3.3 V rail stays in range. 10 us acquisition suits
     * a resistor divider's source impedance.
     */
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;
    NRF_SAADC->CH[0].PSELP = pselp;
    NRF_SAADC->CH[0].CONFIG =
        (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
        (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
        (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
        (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
        (SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos) |
        (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |
        (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);

    NRF_SAADC->RESULT.PTR = (uint32_t)&result;
    NRF_SAADC->RESULT.MAXCNT = 1U;

    NRF_SAADC->EVENTS_STARTED = 0U;
    NRF_SAADC->EVENTS_END = 0U;
    NRF_SAADC->EVENTS_STOPPED = 0U;

    NRF_SAADC->TASKS_START = 1U;
    if (!wait_event(&NRF_SAADC->EVENTS_STARTED)) {
        goto stop;
    }

    NRF_SAADC->TASKS_SAMPLE = 1U;
    if (!wait_event(&NRF_SAADC->EVENTS_END)) {
        goto stop;
    }

    completed = true;

stop:
    /* STOP is also the bounded cleanup path after a partial conversion. This
     * leaves the peripheral in a known state for the next caller; failure to
     * observe STOPPED is harmless because ENABLE is cleared immediately below. */
    NRF_SAADC->TASKS_STOP = 1U;
    (void)wait_event(&NRF_SAADC->EVENTS_STOPPED);

    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_NC;
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;

    k_mutex_unlock(&saadc_lock);

    return completed ? result : G4B_SAADC_READ_FAILED;
}
