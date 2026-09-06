/* SPDX-License-Identifier: MIT */
#include "apex_hop_link.h"
#include <string.h>

enum { MAP_ACK = 0x4d, SYNC_ACK = 0x49, START_SLOT = 5 };
static const uint8_t channels[] = {6, 26, 50, 74};

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t n)
{
    for (unsigned int i = 0; i < 4; i++) p[i] = n >> (8 * i);
}

int apex_hop_link_init(struct apex_hop_link *l, const struct apex_connection *c, uint8_t rendezvous)
{
    if (!l || rendezvous > 80) return APEX_PACKET_INVALID;
    memset(l, 0, sizeof(*l));
    l->rendezvous = rendezvous;
    return apex_hop_init(&l->clock, c);
}

int apex_hop_link_receive(struct apex_hop_link *l, struct apex_connection *c,
                          const uint8_t *in, size_t length, uint64_t received_us,
                          uint8_t *out, size_t capacity)
{
    if (!l || !c || !in || length < APEX_PACKET_HEADER_SIZE ||
        c->state != APEX_CONNECTION_ESTABLISHED || !l->clock.initialized ||
        l->clock.role != c->role || memcmp(l->clock.session_id, c->session.id, 8)) {
        return APEX_CONNECTION_STATE_ERROR;
    }
    if (c->role == APEX_RECEIVER) {
        uint8_t ack[10] = {0, 1};
        int rc;
        size_t n;
        if (in[1] == APEX_PACKET_CHANNEL_MAP) {
            rc = apex_hop_accept_map(&l->clock, c, in, length);
            ack[0] = MAP_ACK;
            n = 6;
        } else if (in[1] == APEX_PACKET_CONTROL) {
            rc = apex_hop_accept_sync(&l->clock, c, in, length, received_us);
            ack[0] = SYNC_ACK;
            write32(ack + 6, l->clock.sync_counter);
            n = 10;
        } else return APEX_PACKET_INVALID;
        if (rc) return rc;
        write32(ack + 2, l->clock.generation);
        return apex_connection_encode(c, APEX_PACKET_CONTROL, ack, n, out, capacity);
    }
    uint8_t type, ack[APEX_PACKET_PAYLOAD_MAX];
    int n = apex_connection_decode(c, in, length, &type, ack, sizeof(ack));
    if (n < 0) return n;
    if (type != APEX_PACKET_CONTROL || n < 6 || ack[1] != 1 ||
        !l->clock.count || read32(ack + 2) != l->clock.generation) return APEX_PACKET_INVALID;
    if (ack[0] == MAP_ACK && n == 6) {
        if (!l->map_acked) {
            int rc = apex_hop_begin(&l->clock, received_us, 0);
            if (rc) return rc;
            l->map_acked = 1;
        }
        return 0;
    }
    if (ack[0] == SYNC_ACK && n == 10 && l->first_sync &&
        read32(ack + 6) >= l->first_sync && read32(ack + 6) <= l->last_sync) {
        l->sync_acked = 1;
        if (read32(ack + 6) > l->acked_sync) l->acked_sync = read32(ack + 6);
        return 0;
    }
    return APEX_PACKET_INVALID;
}

int apex_hop_link_next(struct apex_hop_link *l, struct apex_connection *c,
                       uint64_t scheduled_end_us, uint8_t *out, size_t capacity)
{
    if (!l || !c || c->role != APEX_KEYBOARD) return APEX_CONNECTION_STATE_ERROR;
    if (!l->map_acked) {
        return apex_hop_offer_map(&l->clock, c, channels, sizeof(channels), 1, out, capacity);
    }
    uint32_t slot, phase;
    int rc = apex_hop_position(&l->clock, scheduled_end_us, &slot, &phase);
    if (rc) return rc;
    if (slot >= START_SLOT && !l->sync_acked) return APEX_HOP_EXPIRED;
    if (slot < START_SLOT || slot >= l->next_sync_slot) {
        int n = apex_hop_emit_sync(&l->clock, c, scheduled_end_us, out, capacity);
        if (n > 0) {
            l->last_sync = read32(out + 4);
            if (!l->first_sync) l->first_sync = l->last_sync;
            l->next_sync_slot = (uint64_t)slot + 1;
        }
        return n;
    }
    return 0;
}

int apex_hop_link_channel(const struct apex_hop_link *l, uint64_t now_us)
{
    if (!l || !l->clock.initialized) return APEX_CONNECTION_STATE_ERROR;
    if (!l->clock.running) return l->rendezvous;
    uint32_t slot, phase;
    int rc = apex_hop_position(&l->clock, now_us, &slot, &phase);
    if (rc) return rc;
    if (slot < START_SLOT) return l->rendezvous;
    if (l->clock.role == APEX_KEYBOARD && !l->sync_acked) return APEX_HOP_EXPIRED;
    return apex_hop_channel_at(&l->clock, now_us);
}

bool apex_hop_link_ready(const struct apex_hop_link *l, uint64_t now_us)
{
    if (!l || !l->clock.running) return false;
    uint32_t slot, phase;
    return !apex_hop_position(&l->clock, now_us, &slot, &phase) && slot >= START_SLOT &&
           (l->clock.role == APEX_RECEIVER || l->sync_acked);
}

bool apex_hop_link_window(const struct apex_hop_link *l, uint64_t now_us)
{
    if (!l || !l->clock.running) return true;
    uint32_t slot, phase;
    if (apex_hop_position(&l->clock, now_us, &slot, &phase)) return false;
    /* Leave room for the 2 ms scheduled TX lead and the receiver's reply. */
    return slot < START_SLOT - 1 || (phase >= 3000 && phase < 13000);
}

bool apex_hop_link_input_window(const struct apex_hop_link *l, uint64_t now_us)
{
    if (!apex_hop_link_ready(l, now_us)) return false;
    uint32_t slot, phase;
    if (apex_hop_position(&l->clock, now_us, &slot, &phase)) return false;
    /* Immediate packets need no scheduled TX lead. Keep 4 ms before hopping
     * for encryption, airtime and the reply; USB completion may arrive later. */
    return phase >= 3000 && phase < 16000;
}

uint8_t apex_hop_discovery_channel(enum apex_role role, uint64_t elapsed_us)
{
    static const uint8_t search[] = {74, 6, 50, 26};
    return search[(elapsed_us / (role == APEX_KEYBOARD ? 300000u : 40000u)) % 4];
}
