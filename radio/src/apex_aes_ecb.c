/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/kernel.h>
#include <tinycrypt/aes.h>
#include <hal/nrf_ecb.h>
#include <zephyr/sys/atomic.h>
#include "apex_aes.h"

static struct k_spinlock ecb_lock;
static atomic_t enabled, blocks, fallbacks;

void apex_aes_enable(void) { atomic_set(&enabled, 1); }
uint32_t apex_aes_blocks(void) { return atomic_get(&blocks); }
uint32_t apex_aes_fallbacks(void) { return atomic_get(&fallbacks); }

int __real_tc_aes_encrypt(uint8_t *out, const uint8_t *in, const TCAesKeySched_t schedule);

/* Enable only after Bluetooth is shut down. CCM, HMAC and packet framing
 * remain the pinned library implementations. */
int __wrap_tc_aes_encrypt(uint8_t *out, const uint8_t *in, const TCAesKeySched_t schedule)
{
    if (!out || !in || !schedule) return 0;
    if (!atomic_get(&enabled) || k_is_in_isr()) return __real_tc_aes_encrypt(out, in, schedule);
    uint8_t dma[48] __aligned(4);
    for (unsigned int i = 0; i < 16; ++i) {
        dma[i] = schedule->words[i / 4] >> (24 - 8 * (i % 4));
    }
    memcpy(dma + 16, in, 16);
    k_spinlock_key_t lock = k_spin_lock(&ecb_lock);
    NRF_ECB->ECBDATAPTR = (uint32_t)dma;
    NRF_ECB->EVENTS_ENDECB = 0;
    NRF_ECB->EVENTS_ERRORECB = 0;
    uint32_t start = k_cycle_get_32();
    NRF_ECB->TASKS_STARTECB = 1;
    while (!NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB &&
           k_cycle_get_32() - start < k_us_to_cyc_ceil32(100)) {
    }
    bool ok = NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB;
    NRF_ECB->TASKS_STOPECB = 1;
    __DSB();
    if (ok) memcpy(out, dma + 32, 16);
    NRF_ECB->ECBDATAPTR = 0;
    k_spin_unlock(&ecb_lock, lock);
    volatile uint8_t *clear = dma;
    for (unsigned int i = 0; i < sizeof(dma); ++i) clear[i] = 0;
    /* TinyCrypt CCM does not propagate AES errors. Fall back to the reference
     * implementation if hardware times out, rather than use a partial block. */
    if (ok) atomic_inc(&blocks);
    else atomic_inc(&fallbacks);
    return ok ? 1 : __real_tc_aes_encrypt(out, in, schedule);
}
