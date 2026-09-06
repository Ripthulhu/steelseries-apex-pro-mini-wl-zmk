/* SPDX-License-Identifier: MIT */
#include "apex_connection.h"
#include <string.h>
#include <tinycrypt/hmac.h>

enum { HELLO = 0x80, CHALLENGE, CONFIRM, READY };

static void clear(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

void apex_connection_clear(struct apex_connection *c)
{
    if (c) clear(c, sizeof(*c));
}

int apex_connection_init(struct apex_connection *c, const uint8_t key[16],
                         const uint8_t id[8], enum apex_role role)
{
    if (!c) return APEX_PACKET_INVALID;
    apex_connection_clear(c);
    if (!key || !id || role < APEX_KEYBOARD || role > APEX_RECEIVER) return APEX_PACKET_INVALID;
    memcpy(c->key, key, 16);
    memcpy(c->pair_id, id, 8);
    c->role = role;
    c->configured = 1;
    return 0;
}

int apex_connection_start(struct apex_connection *c, const uint8_t nonce[16])
{
    if (!c || !c->configured || !nonce || c->role > APEX_RECEIVER) return APEX_PACKET_INVALID;
    /* Refuse accidental local nonce reuse, even if the last attempt timed out. */
    uint8_t *local = c->role == APEX_KEYBOARD ? c->keyboard_nonce : c->receiver_nonce;
    if (!memcmp(local, nonce, 16)) return APEX_PACKET_INVALID;
    apex_session_clear(&c->session);
    memset(c->keyboard_nonce, 0, 16);
    memset(c->receiver_nonce, 0, 16);
    memcpy(local, nonce, 16);
    c->state = c->role == APEX_KEYBOARD ? APEX_CONNECTION_WAIT_CHALLENGE : APEX_CONNECTION_WAIT_HELLO;
    return 0;
}

static int tag(const struct apex_connection *c, const uint8_t *in, uint8_t out[32])
{
    static const uint8_t domain[] = "apex-radio-v1-handshake";
    struct tc_hmac_state_struct h;
    int ok = tc_hmac_set_key(&h, c->key, 16) && tc_hmac_init(&h) &&
             tc_hmac_update(&h, domain, sizeof(domain) - 1) &&
             tc_hmac_update(&h, in, 42) && tc_hmac_final(out, 32, &h);
    clear(&h, sizeof(h));
    return ok;
}

static int emit(const struct apex_connection *c, uint8_t kind, uint8_t *out, size_t capacity)
{
    uint8_t wire[APEX_HANDSHAKE_SIZE] = {APEX_PROTOCOL_VERSION, kind}, mac[32];
    if (!out || capacity < sizeof(wire)) return APEX_PACKET_INVALID;
    memcpy(wire + 2, c->pair_id, 8);
    memcpy(wire + 10, c->keyboard_nonce, 16);
    if (kind != HELLO) memcpy(wire + 26, c->receiver_nonce, 16);
    if (!tag(c, wire, mac)) return APEX_PACKET_AUTH_FAILED;
    memcpy(wire + 42, mac, 16);
    clear(mac, sizeof(mac));
    memcpy(out, wire, sizeof(wire));
    return sizeof(wire);
}

int apex_connection_request(const struct apex_connection *c, uint8_t *out, size_t capacity)
{
    if (!c || !c->configured) return APEX_PACKET_INVALID;
    if (c->state == APEX_CONNECTION_WAIT_CHALLENGE) return emit(c, HELLO, out, capacity);
    if (c->state == APEX_CONNECTION_WAIT_READY) return emit(c, CONFIRM, out, capacity);
    return APEX_CONNECTION_STATE_ERROR;
}

int apex_connection_receive(struct apex_connection *c, const uint8_t *in, size_t length,
                            uint8_t *out, size_t capacity)
{
    uint8_t mac[32];
    if (!c || !c->configured || !in || !out || capacity < APEX_HANDSHAKE_SIZE || length != APEX_HANDSHAKE_SIZE ||
        in[0] != APEX_PROTOCOL_VERSION || in[1] < HELLO || in[1] > READY ||
        memcmp(in + 2, c->pair_id, 8)) return APEX_PACKET_INVALID;
    if (!tag(c, in, mac)) return APEX_PACKET_AUTH_FAILED;
    uint8_t difference = 0;
    for (unsigned int i = 0; i < 16; ++i) difference |= mac[i] ^ in[42 + i];
    clear(mac, sizeof(mac));
    if (difference) return APEX_PACKET_AUTH_FAILED;

    if (c->role == APEX_RECEIVER && in[1] == HELLO) {
        static const uint8_t zero[16];
        if (memcmp(in + 26, zero, 16)) return APEX_PACKET_INVALID;
        if (c->state == APEX_CONNECTION_WAIT_HELLO) {
            memcpy(c->keyboard_nonce, in + 10, 16);
            int rc = apex_session_init(&c->session, c->key, c->keyboard_nonce,
                                       c->receiver_nonce, APEX_RECEIVER);
            if (rc) return rc;
            c->state = APEX_CONNECTION_WAIT_CONFIRM;
        } else if (c->state != APEX_CONNECTION_WAIT_CONFIRM || memcmp(in + 10, c->keyboard_nonce, 16)) {
            return APEX_CONNECTION_STATE_ERROR;
        }
        return emit(c, CHALLENGE, out, capacity);
    }
    if (c->role == APEX_KEYBOARD && in[1] == CHALLENGE) {
        if (memcmp(in + 10, c->keyboard_nonce, 16)) return APEX_CONNECTION_STATE_ERROR;
        if (c->state == APEX_CONNECTION_WAIT_CHALLENGE) {
            memcpy(c->receiver_nonce, in + 26, 16);
            int rc = apex_session_init(&c->session, c->key, c->keyboard_nonce,
                                       c->receiver_nonce, APEX_KEYBOARD);
            if (rc) return rc;
            c->state = APEX_CONNECTION_WAIT_READY;
        } else if (c->state != APEX_CONNECTION_WAIT_READY || memcmp(in + 26, c->receiver_nonce, 16)) {
            return APEX_CONNECTION_STATE_ERROR;
        }
        return emit(c, CONFIRM, out, capacity);
    }
    if (memcmp(in + 10, c->keyboard_nonce, 16) || memcmp(in + 26, c->receiver_nonce, 16)) {
        return APEX_CONNECTION_STATE_ERROR;
    }
    if (c->role == APEX_RECEIVER && in[1] == CONFIRM &&
        (c->state == APEX_CONNECTION_WAIT_CONFIRM || c->state == APEX_CONNECTION_ESTABLISHED)) {
        c->state = APEX_CONNECTION_ESTABLISHED;
        return emit(c, READY, out, capacity);
    }
    if (c->role == APEX_KEYBOARD && in[1] == READY &&
        (c->state == APEX_CONNECTION_WAIT_READY || c->state == APEX_CONNECTION_ESTABLISHED)) {
        c->state = APEX_CONNECTION_ESTABLISHED;
        return 0;
    }
    return APEX_CONNECTION_STATE_ERROR;
}

int apex_connection_encode(struct apex_connection *c, uint8_t type, const uint8_t *payload,
                           size_t length, uint8_t *out, size_t capacity)
{
    if (!c || c->state != APEX_CONNECTION_ESTABLISHED) return APEX_CONNECTION_STATE_ERROR;
    return apex_packet_encode(&c->session, type, payload, length, out, capacity);
}

int apex_connection_decode(struct apex_connection *c, const uint8_t *in, size_t length,
                           uint8_t *type, uint8_t *out, size_t capacity)
{
    if (!c || c->state != APEX_CONNECTION_ESTABLISHED) return APEX_CONNECTION_STATE_ERROR;
    return apex_packet_decode(&c->session, in, length, type, out, capacity);
}
