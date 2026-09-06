/* SPDX-License-Identifier: MIT */
#include "apex_hop.h"
#include <string.h>

enum { FORMAT = 1, SYNC = 0x48 };

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(uint8_t *p, uint32_t n)
{
    for (unsigned int i = 0; i < 4; i++) p[i] = n >> (i * 8);
}

static bool bound(const struct apex_hop *h, const struct apex_connection *c)
{
    return h && c && h->initialized && c->state == APEX_CONNECTION_ESTABLISHED &&
           c->role == h->role && !memcmp(h->session_id, c->session.id, 8);
}

static bool valid_map(const uint8_t *channels, size_t count, uint32_t generation)
{
    if (!channels || !generation || count < 4 || count > APEX_HOP_CHANNELS_MAX) return false;
    for (size_t i = 0; i < count; i++) {
        if (channels[i] < 2 || channels[i] > 80 ||
            (i && channels[i] < channels[i - 1] + 2)) return false;
    }
    return true;
}

static unsigned int gcd(unsigned int a, unsigned int b)
{
    while (b) {
        unsigned int remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

static void install(struct apex_hop *h, const uint8_t *channels, size_t count, uint32_t generation)
{
    memset(h->channels, 0, sizeof(h->channels));
    memcpy(h->channels, channels, count);
    h->count = count;
    h->generation = generation;
    h->offset = read32(h->session_id) % count;
    h->stride = 1 + read32(h->session_id + 4) % (count - 1);
    while (gcd(h->stride, count) != 1) h->stride = h->stride % (count - 1) + 1;
}

int apex_hop_init(struct apex_hop *h, const struct apex_connection *c)
{
    if (!h) return APEX_PACKET_INVALID;
    memset(h, 0, sizeof(*h));
    if (!c || c->state != APEX_CONNECTION_ESTABLISHED || !c->session.ready ||
        c->role > APEX_RECEIVER) return APEX_CONNECTION_STATE_ERROR;
    memcpy(h->session_id, c->session.id, 8);
    h->role = c->role;
    h->initialized = 1;
    return 0;
}

static bool acceptable(const struct apex_hop *h, const uint8_t *channels,
                       size_t count, uint32_t generation)
{
    return valid_map(channels, count, generation) &&
           (generation > h->generation || (generation == h->generation &&
            count == h->count && !memcmp(channels, h->channels, count)));
}

int apex_hop_offer_map(struct apex_hop *h, struct apex_connection *c,
                      const uint8_t *channels, size_t count, uint32_t generation,
                      uint8_t *packet, size_t capacity)
{
    if (!bound(h, c) || h->role != APEX_KEYBOARD || h->running) return APEX_CONNECTION_STATE_ERROR;
    if (!acceptable(h, channels, count, generation)) return APEX_PACKET_INVALID;
    uint8_t payload[6 + APEX_HOP_CHANNELS_MAX] = {FORMAT};
    write32(payload + 1, generation);
    payload[5] = count;
    memcpy(payload + 6, channels, count);
    int n = apex_connection_encode(c, APEX_PACKET_CHANNEL_MAP, payload, count + 6, packet, capacity);
    if (n > 0) install(h, channels, count, generation);
    return n;
}

int apex_hop_accept_map(struct apex_hop *h, struct apex_connection *c,
                       const uint8_t *packet, size_t length)
{
    if (!bound(h, c) || h->role != APEX_RECEIVER || h->running) return APEX_CONNECTION_STATE_ERROR;
    uint8_t type, payload[APEX_PACKET_PAYLOAD_MAX];
    int n = apex_connection_decode(c, packet, length, &type, payload, sizeof(payload));
    if (n < 0) return n;
    if (type != APEX_PACKET_CHANNEL_MAP || n < 6 || payload[0] != FORMAT ||
        n != payload[5] + 6 || !acceptable(h, payload + 6, payload[5], read32(payload + 1))) {
        return APEX_PACKET_INVALID;
    }
    install(h, payload + 6, payload[5], read32(payload + 1));
    return 0;
}

int apex_hop_begin(struct apex_hop *h, uint64_t local_us, uint32_t slot)
{
    if (!h || !h->initialized || !h->count || h->role != APEX_KEYBOARD || h->running) {
        return APEX_CONNECTION_STATE_ERROR;
    }
    h->anchor_us = local_us;
    h->anchor_slot = slot;
    h->phase_us = 0;
    h->running = 1;
    return 0;
}

int apex_hop_position(const struct apex_hop *h, uint64_t local_us, uint32_t *slot, uint32_t *phase)
{
    if (!h || !slot || !phase || !h->initialized || !h->running || !h->count || local_us < h->anchor_us) {
        return APEX_CONNECTION_STATE_ERROR;
    }
    uint64_t age = local_us - h->anchor_us;
    if (h->role == APEX_RECEIVER && age >= APEX_HOP_HOLDOVER_US) return APEX_HOP_EXPIRED;
    /* Check before adding phase so even UINT64_MAX timestamps cannot wrap. */
    uint64_t available = ((uint64_t)UINT32_MAX - h->anchor_slot + 1) * APEX_HOP_SLOT_US;
    if (age >= available - h->phase_us) return APEX_PACKET_REKEY;
    uint64_t elapsed = age + h->phase_us;
    *slot = h->anchor_slot + elapsed / APEX_HOP_SLOT_US;
    *phase = elapsed % APEX_HOP_SLOT_US;
    return 0;
}

int apex_hop_channel_at(const struct apex_hop *h, uint64_t local_us)
{
    uint32_t slot, phase;
    int rc = apex_hop_position(h, local_us, &slot, &phase);
    if (rc) return rc;
    return h->channels[(h->offset + (slot % h->count) * h->stride) % h->count];
}

int apex_hop_emit_sync(struct apex_hop *h, struct apex_connection *c,
                       uint64_t local_us, uint8_t *packet, size_t capacity)
{
    if (!bound(h, c) || h->role != APEX_KEYBOARD) return APEX_CONNECTION_STATE_ERROR;
    uint32_t slot, phase;
    int rc = apex_hop_position(h, local_us, &slot, &phase);
    if (rc) return rc;
    uint8_t payload[12] = {SYNC, FORMAT};
    write32(payload + 2, h->generation);
    write32(payload + 6, slot);
    payload[10] = phase;
    payload[11] = phase >> 8;
    return apex_connection_encode(c, APEX_PACKET_CONTROL, payload, sizeof(payload), packet, capacity);
}

int apex_hop_accept_sync(struct apex_hop *h, struct apex_connection *c,
                         const uint8_t *packet, size_t length, uint64_t local_us)
{
    if (!bound(h, c) || h->role != APEX_RECEIVER || !h->count) return APEX_CONNECTION_STATE_ERROR;
    if (h->running && local_us >= h->anchor_us &&
        local_us - h->anchor_us >= APEX_HOP_HOLDOVER_US) return APEX_HOP_EXPIRED;
    uint8_t type, payload[APEX_PACKET_PAYLOAD_MAX];
    int n = apex_connection_decode(c, packet, length, &type, payload, sizeof(payload));
    if (n < 0) return n;
    if (type != APEX_PACKET_CONTROL || n != 12 || payload[0] != SYNC || payload[1] != FORMAT ||
        read32(payload + 2) != h->generation) return APEX_PACKET_INVALID;
    uint32_t slot = read32(payload + 6), phase = payload[10] | (uint32_t)payload[11] << 8;
    uint32_t counter = read32(packet + 4);
    if (phase >= APEX_HOP_SLOT_US || counter <= h->sync_counter ||
        (h->running && (local_us < h->anchor_us || slot < h->anchor_slot ||
         (slot == h->anchor_slot && phase < h->phase_us)))) return APEX_PACKET_INVALID;
    if (h->running) {
        uint64_t predicted = (uint64_t)h->anchor_slot * APEX_HOP_SLOT_US +
                             h->phase_us + local_us - h->anchor_us;
        uint64_t advertised = (uint64_t)slot * APEX_HOP_SLOT_US + phase;
        uint64_t difference = predicted > advertised ? predicted - advertised : advertised - predicted;
        if (difference > APEX_HOP_SYNC_SKEW_US) return APEX_PACKET_INVALID;
    }
    h->anchor_us = local_us;
    h->anchor_slot = slot;
    h->phase_us = phase;
    h->sync_counter = counter;
    h->running = 1;
    return 0;
}
