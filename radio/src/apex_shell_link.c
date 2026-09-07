/* SPDX-License-Identifier: MIT */
#include "apex_shell_link.h"
#include "apex_radio_input.h"
#include <zephyr/kernel.h>

static struct apex_stream stream;
static struct k_spinlock lock;
static uint32_t generation = 1;
static atomic_t bulk_at, bulk_recent;

void apex_shell_bulk_touch(void)
{
    atomic_set(&bulk_at, k_uptime_get_32());
    atomic_set(&bulk_recent, 1);
}

bool apex_shell_bulk_active(void)
{
    return atomic_get(&bulk_recent) &&
           k_uptime_get_32() - (uint32_t)atomic_get(&bulk_at) < 2000u;
}
__weak void apex_shell_rx_ready(void) {}

uint32_t apex_shell_generation(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    uint32_t value = generation;
    k_spin_unlock(&lock, key);
    return value;
}

void apex_shell_link_reset(void)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    apex_stream_reset(&stream);
    atomic_set(&bulk_recent, 0);
    generation++;
    k_spin_unlock(&lock, key);
    apex_shell_rx_ready();
}

int apex_shell_link_pack(uint8_t *packet)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    int length = apex_stream_pack(&stream, packet);
    k_spin_unlock(&lock, key);
    return length;
}

int apex_shell_link_receive(const uint8_t *packet, size_t length)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    int rc = apex_stream_receive(&stream, packet, length);
    k_spin_unlock(&lock, key);
    if (rc > 0) apex_shell_rx_ready();
    return rc;
}

int apex_shell_send(uint32_t expected, const uint8_t *data, size_t length, int64_t deadline)
{
    for (;;) {
        if (!apex_radio_input_connected()) return -ENOTCONN;
        if (k_uptime_get() >= deadline) return -ETIMEDOUT;
        k_spinlock_key_t key = k_spin_lock(&lock);
        int rc = expected == generation ? apex_stream_put(&stream, data, length) : -ECANCELED;
        k_spin_unlock(&lock, key);
        if (rc) return rc < 0 ? rc : 0;
        k_sleep(K_MSEC(1));
    }
}

int apex_shell_read(uint32_t expected, uint8_t *data)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    int rc = expected == generation ? apex_stream_get(&stream, data) : -ECANCELED;
    k_spin_unlock(&lock, key);
    return rc;
}
