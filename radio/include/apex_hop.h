/* SPDX-License-Identifier: MIT */
#ifndef APEX_HOP_H
#define APEX_HOP_H
#include "apex_connection.h"
#include <stdbool.h>

#define APEX_HOP_CHANNELS_MAX 16
#define APEX_HOP_SLOT_US 20000u
#define APEX_HOP_HOLDOVER_US 100000u
#define APEX_HOP_SYNC_SKEW_US 2000u
#define APEX_HOP_EXPIRED (-6)

struct apex_hop {
    uint64_t anchor_us;
    uint32_t generation, anchor_slot, phase_us, sync_counter;
    uint8_t session_id[8], channels[APEX_HOP_CHANNELS_MAX];
    uint8_t count, offset, stride, role, initialized, running;
};

/* One schedule per authenticated session. Changing a running map requires a
 * fresh session; live map replacement needs a separate recovery protocol. */
int apex_hop_init(struct apex_hop *h, const struct apex_connection *c);
int apex_hop_offer_map(struct apex_hop *h, struct apex_connection *c,
                      const uint8_t *channels, size_t count, uint32_t generation,
                      uint8_t *packet, size_t capacity);
int apex_hop_accept_map(struct apex_hop *h, struct apex_connection *c,
                       const uint8_t *packet, size_t length);
/* The keyboard owns slot time. The receiver only sets its clock from an
 * authenticated sync packet. local_us must be a monotonic 64-bit timestamp. */
int apex_hop_begin(struct apex_hop *h, uint64_t local_us, uint32_t slot);
int apex_hop_emit_sync(struct apex_hop *h, struct apex_connection *c,
                       uint64_t local_us, uint8_t *packet, size_t capacity);
int apex_hop_accept_sync(struct apex_hop *h, struct apex_connection *c,
                         const uint8_t *packet, size_t length, uint64_t local_us);
/* Returns frequency offset in MHz, or an error. A receiver refuses to predict
 * channels after 100 ms without sync; the caller must then reconnect. */
int apex_hop_channel_at(const struct apex_hop *h, uint64_t local_us);
int apex_hop_position(const struct apex_hop *h, uint64_t local_us,
                      uint32_t *slot, uint32_t *phase);
#endif
