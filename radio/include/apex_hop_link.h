/* SPDX-License-Identifier: MIT */
#ifndef APEX_HOP_LINK_H
#define APEX_HOP_LINK_H
#include "apex_hop.h"

struct apex_hop_link {
    struct apex_hop clock;
    uint64_t next_sync_slot;
    uint32_t first_sync, last_sync;
    uint8_t rendezvous, map_acked, sync_acked;
};

int apex_hop_link_init(struct apex_hop_link *l, const struct apex_connection *c, uint8_t rendezvous);
/* CONTROL/CHANNEL_MAP packets only. Decode exactly once, in this function. */
int apex_hop_link_receive(struct apex_hop_link *l, struct apex_connection *c,
                          const uint8_t *in, size_t length, uint64_t received_us,
                          uint8_t *out, size_t capacity);
/* Keyboard: positive result is a control packet, zero permits an input packet.
 * scheduled_end_us is the planned on-air END event, not the thread wake time. */
int apex_hop_link_next(struct apex_hop_link *l, struct apex_connection *c,
                       uint64_t scheduled_end_us, uint8_t *out, size_t capacity);
int apex_hop_link_channel(const struct apex_hop_link *l, uint64_t now_us);
bool apex_hop_link_ready(const struct apex_hop_link *l, uint64_t now_us);
bool apex_hop_link_window(const struct apex_hop_link *l, uint64_t now_us);
uint8_t apex_hop_discovery_channel(enum apex_role role, uint64_t elapsed_us);
#endif
