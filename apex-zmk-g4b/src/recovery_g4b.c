/* SPDX-License-Identifier: MIT
 *
 * Software-triggered entry to the current Adafruit nRF52 bootloader. The loader
 * reads GPREGRET and enters UF2/serial DFU when its low byte is 0x57. This is the
 * same mechanism used by behavior_dfu_g4b.c and dfu_trigger_g4b.c.
 */

#include <zephyr/kernel.h>

#include <nrfx.h>

#include "recovery_g4b.h"

#define G4B_DFU_MAGIC_UF2_RESET 0x57u

void g4b_enter_recovery(void)
{
    NRF_POWER->GPREGRET = G4B_DFU_MAGIC_UF2_RESET;
    __DSB();
    __ISB();
    NVIC_SystemReset();
}
