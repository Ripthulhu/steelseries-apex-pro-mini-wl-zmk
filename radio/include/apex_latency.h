/* SPDX-License-Identifier: MIT */
#ifndef APEX_LATENCY_H
#define APEX_LATENCY_H
#include <stdint.h>

/* Callers serialize updates and snapshots. Buckets are non-cumulative. */
struct apex_latency {
    uint64_t total_us;
    uint32_t count, min_us, max_us, buckets[6];
};

static inline void apex_latency_add(struct apex_latency *s, uint32_t us)
{
    static const uint32_t limits[] = {1000, 2000, 5000, 10000, 20000};
    if (s->count == UINT32_MAX) return;
    if (!s->count || us < s->min_us) s->min_us = us;
    if (us > s->max_us) s->max_us = us;
    s->count++;
    s->total_us += us;
    unsigned int bucket = 0;
    while (bucket < 5 && us > limits[bucket]) bucket++;
    s->buckets[bucket]++;
}
#endif
