/* SPDX-License-Identifier: MIT */
#include "apex_packet.h"
#include "apex_connection.h"
#include "apex_hop.h"
#include "apex_hop_link.h"

size_t fixture_session_size(void) { return sizeof(struct apex_session); }
void fixture_exhaust_counter(struct apex_session *s) { s->tx_counter = UINT32_MAX; }
size_t fixture_connection_size(void) { return sizeof(struct apex_connection); }
size_t fixture_hop_size(void) { return sizeof(struct apex_hop); }
int fixture_hop_begin(struct apex_hop *h, uint32_t low, uint32_t high, uint32_t slot)
{
    return apex_hop_begin(h, (uint64_t)high << 32 | low, slot);
}
int fixture_hop_channel(struct apex_hop *h, uint32_t low, uint32_t high)
{
    return apex_hop_channel_at(h, (uint64_t)high << 32 | low);
}
int fixture_hop_sync(struct apex_hop *h, struct apex_connection *c, uint32_t now,
                     uint8_t *out, size_t capacity)
{
    return apex_hop_emit_sync(h, c, now, out, capacity);
}
int fixture_hop_receive(struct apex_hop *h, struct apex_connection *c,
                        const uint8_t *packet, size_t length, uint32_t now)
{
    return apex_hop_accept_sync(h, c, packet, length, now);
}
int fixture_hl_next(struct apex_hop_link *l, struct apex_connection *c,
                     uint32_t now, uint8_t *out, size_t capacity)
{
    return apex_hop_link_next(l, c, now, out, capacity);
}
int fixture_hl_receive(struct apex_hop_link *l, struct apex_connection *c,
                        const uint8_t *in, size_t length, uint32_t now,
                        uint8_t *out, size_t capacity)
{
    return apex_hop_link_receive(l, c, in, length, now, out, capacity);
}
int fixture_hl_channel(struct apex_hop_link *l, uint32_t now)
{
    return apex_hop_link_channel(l, now);
}
int fixture_hl_window(struct apex_hop_link *l, uint32_t now)
{
    return apex_hop_link_window(l, now);
}
int fixture_discovery(enum apex_role role, uint32_t elapsed)
{
    return apex_hop_discovery_channel(role, elapsed);
}
