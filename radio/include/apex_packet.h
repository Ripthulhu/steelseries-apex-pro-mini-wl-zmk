/* SPDX-License-Identifier: MIT */
#ifndef APEX_PACKET_H
#define APEX_PACKET_H

#include <stddef.h>
#include <stdint.h>
#include <tinycrypt/aes.h>

#define APEX_PROTOCOL_VERSION 1
#define APEX_PACKET_HEADER_SIZE 16
#define APEX_PACKET_TAG_SIZE 8
#define APEX_PACKET_PAYLOAD_MAX 64
#define APEX_PACKET_MAX (APEX_PACKET_HEADER_SIZE + APEX_PACKET_PAYLOAD_MAX + APEX_PACKET_TAG_SIZE)

enum apex_role { APEX_KEYBOARD = 0, APEX_RECEIVER = 1 };
enum apex_packet_type {
    APEX_PACKET_INPUT = 1,
    APEX_PACKET_ACK = 2,
    APEX_PACKET_CONTROL = 3,
    APEX_PACKET_KEEPALIVE = 4,
    APEX_PACKET_CHANNEL_MAP = 5,
};
enum apex_packet_result {
    APEX_PACKET_OK = 0,
    APEX_PACKET_INVALID = -1,
    APEX_PACKET_AUTH_FAILED = -2,
    APEX_PACKET_REPLAY = -3,
    APEX_PACKET_REKEY = -4,
};

struct apex_session {
    struct tc_aes_key_sched_struct aes;
    uint8_t id[8];
    uint32_t tx_counter;
    uint32_t rx_highest;
    uint64_t rx_seen;
    uint8_t role;
    uint8_t ready;
};

/* Both nonces must come from the authenticated handshake, with a new local
 * 16-byte cryptographic nonce on every connection attempt. Never restore a
 * session across reset or call this again with the same nonce pair. */
int apex_session_init(struct apex_session *s, const uint8_t pairing_key[16],
                      const uint8_t keyboard_nonce[16], const uint8_t receiver_nonce[16],
                      enum apex_role role);
void apex_session_clear(struct apex_session *s);
/* Callers serialize access to a session. Input and output buffers must not overlap. */
int apex_packet_encode(struct apex_session *s, uint8_t type, const uint8_t *payload,
                       size_t length, uint8_t *packet, size_t capacity);
/* Returns payload length on success. Failed packets do not change rx state,
 * type or output. A replay is never delivered, including an authenticated retry. */
int apex_packet_decode(struct apex_session *s, const uint8_t *packet, size_t length,
                       uint8_t *type, uint8_t *payload, size_t capacity);

#endif
