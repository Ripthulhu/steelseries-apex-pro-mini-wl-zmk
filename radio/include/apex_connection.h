/* SPDX-License-Identifier: MIT */
#ifndef APEX_CONNECTION_H
#define APEX_CONNECTION_H
#include "apex_packet.h"

#define APEX_HANDSHAKE_SIZE 58
#define APEX_CONNECTION_STATE_ERROR (-5)
enum apex_connection_state {
    APEX_CONNECTION_OFF, APEX_CONNECTION_WAIT_HELLO,
    APEX_CONNECTION_WAIT_CHALLENGE, APEX_CONNECTION_WAIT_CONFIRM,
    APEX_CONNECTION_WAIT_READY, APEX_CONNECTION_ESTABLISHED,
};
struct apex_connection {
    struct apex_session session;
    uint8_t key[16], pair_id[8], keyboard_nonce[16], receiver_nonce[16];
    uint8_t role, state, configured;
};

/* start() consumes a fresh local cryptographic nonce. On timeout, generate a
 * new nonce and start again; do not reset just the packet counters. */
int apex_connection_init(struct apex_connection *c, const uint8_t key[16],
                         const uint8_t pair_id[8], enum apex_role role);
int apex_connection_start(struct apex_connection *c, const uint8_t local_nonce[16]);
void apex_connection_clear(struct apex_connection *c);
int apex_connection_request(const struct apex_connection *c, uint8_t *out, size_t capacity);
int apex_connection_receive(struct apex_connection *c, const uint8_t *in, size_t length,
                            uint8_t *out, size_t capacity);
int apex_connection_encode(struct apex_connection *c, uint8_t type, const uint8_t *payload,
                           size_t length, uint8_t *out, size_t capacity);
int apex_connection_decode(struct apex_connection *c, const uint8_t *in, size_t length,
                           uint8_t *type, uint8_t *out, size_t capacity);
#endif
