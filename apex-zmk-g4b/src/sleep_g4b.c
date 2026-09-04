/* SPDX-License-Identifier: MIT
 *
 * System OFF and its wake sources. See sleep_g4b.h for the safety argument.
 */

#include <zephyr/kernel.h>

#include <nrfx.h>

#include "mode_g4b.h"
#include "pins_g4b.h"
#include "rgb_g4b.h"
#include "sleep_g4b.h"

/* PIN_CNF SENSE field, bits 17:16. 2 = High, 3 = Low, 0 = Disabled. */
#define G4B_PINCNF_SENSE_HIGH (2u << 16)

#define G4B_SLEEP_MODE_PIN 3u /* P0.03, the mode switch divider (AIN1) */

static bool sleep_wake_proven;

void g4b_sleep_arm(void)
{
    /* Input, no pull, buffer connected, sense on the high level. ATTN is
     * already a plain digital input here; this only adds the SENSE field.
     */
    g4b_pin_cfg(G4B_PORT0, G4B_P0_ATTN,
                G4B_PINCNF_DIR_INPUT | G4B_PINCNF_INBUF_CONN |
                    G4B_PINCNF_PULL_NONE | G4B_PINCNF_SENSE_HIGH);

    /* Clear anything latched during bring-up, so the proof below can only come
     * from a sense match observed after this point.
     */
    NRF_P0->LATCH = 0xFFFFFFFFu;
    __DSB();
}

void g4b_sleep_poll_latch(void)
{
    uint32_t latch = NRF_P0->LATCH;

    if ((latch & BIT(G4B_P0_ATTN)) != 0u) {
        sleep_wake_proven = true;
        /* Write-one-to-clear, so the next match re-latches. Only the bits that
         * were seen are cleared - a blanket write could drop a match on
         * another pin between the read and the write.
         */
        NRF_P0->LATCH = latch;
        __DSB();
    }
}

bool g4b_sleep_wake_proven(void)
{
    return sleep_wake_proven;
}

uint32_t g4b_sleep_latch_raw(void)
{
    return NRF_P0->LATCH;
}

void g4b_sleep_enter(void)
{
    uint32_t guard;

    /* LEDs off first. The IS31 holds its PWM page with or without us, so a
     * frame left loaded would stay lit for the whole sleep and defeat the
     * point. The idle blanker has almost certainly done this already; doing it
     * again costs one transfer and removes the ordering assumption.
     */
    g4b_rgb_set_blanked(true);

    /* Second wake source, armed as late as possible: this connects a digital
     * input buffer to a SAADC pin, and the watchdog feeder gates on that pin's
     * analog reading.
     */
    g4b_pin_cfg(G4B_PORT0, (enum g4b_pin)G4B_SLEEP_MODE_PIN,
                G4B_PINCNF_DIR_INPUT | G4B_PINCNF_INBUF_CONN |
                    G4B_PINCNF_PULL_NONE | G4B_PINCNF_SENSE_HIGH);

    /* Clear the latch immediately before committing. DETECT is a level OR
     * across armed pins; if it is high when SYSTEMOFF is written the chip wakes
     * again at once.
     */
    NRF_P0->LATCH = 0xFFFFFFFFu;
    __DSB();

    NRF_POWER->SYSTEMOFF = 1u;
    __DSB();

    /* Return if System OFF fails instead of remaining unresponsive in WFE. The
     * caller records the failure and does not try again during this boot.
     */
    guard = 200000u;
    while (guard-- != 0u) {
        __NOP();
    }
}
