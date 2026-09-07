/* SPDX-License-Identifier: MIT */
#include "apex_packet.h"
#include <string.h>
#include <tinycrypt/ccm_mode.h>
#include <tinycrypt/constants.h>
#include <tinycrypt/hmac.h>

static void wipe(void *data, size_t size)
{
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}

void apex_session_clear(struct apex_session *s)
{
    if (s) wipe(s, sizeof(*s));
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}

int apex_session_init(struct apex_session *s, const uint8_t key[16],
                      const uint8_t kn[16], const uint8_t dn[16], enum apex_role role)
{
    static const uint8_t label[] = "apex-radio-v1-session";
    struct tc_hmac_state_struct hmac;
    uint8_t derived[32];
    if (!s) return APEX_PACKET_INVALID;
    apex_session_clear(s);
    if (!key || !kn || !dn || role > APEX_RECEIVER || role < APEX_KEYBOARD) {
        return APEX_PACKET_INVALID;
    }
    int ok = tc_hmac_set_key(&hmac, key, 16) && tc_hmac_init(&hmac) &&
             tc_hmac_update(&hmac, label, sizeof(label) - 1) &&
             tc_hmac_update(&hmac, kn, 16) && tc_hmac_update(&hmac, dn, 16) &&
             tc_hmac_final(derived, sizeof(derived), &hmac);
    if (ok) {
        ok = tc_aes128_set_encrypt_key(&s->aes, derived);
        memcpy(s->id, derived + 16, sizeof(s->id));
        s->role = role;
        s->tx_counter = 1;
        s->ready = ok;
    }
    wipe(derived, sizeof(derived));
    wipe(&hmac, sizeof(hmac));
    return ok ? APEX_PACKET_OK : APEX_PACKET_AUTH_FAILED;
}

static void make_nonce(uint8_t nonce[13], const uint8_t *packet)
{
    memcpy(nonce, packet + 8, 8);
    memcpy(nonce + 8, packet + 4, 4);
    nonce[12] = packet[2];
}

static int valid_type(uint8_t type)
{
    return type >= APEX_PACKET_INPUT && type <= APEX_PACKET_GAMEPAD;
}

int apex_packet_encode(struct apex_session *s, uint8_t type, const uint8_t *payload,
                       size_t length, uint8_t *packet, size_t capacity)
{
    uint8_t nonce[13];
    struct tc_ccm_mode_struct ccm;
    if (!s || !s->ready || !packet || (!payload && length) ||
        !valid_type(type) || length > APEX_PACKET_PAYLOAD_MAX ||
        capacity < APEX_PACKET_HEADER_SIZE + length + APEX_PACKET_TAG_SIZE) {
        return APEX_PACKET_INVALID;
    }
    if (s->tx_counter == UINT32_MAX) return APEX_PACKET_REKEY;
    packet[0] = APEX_PROTOCOL_VERSION;
    packet[1] = type;
    packet[2] = s->role;
    packet[3] = length;
    write32(packet + 4, s->tx_counter++);
    memcpy(packet + 8, s->id, sizeof(s->id));
    make_nonce(nonce, packet);
    int ok = tc_ccm_config(&ccm, &s->aes, nonce, sizeof(nonce), APEX_PACKET_TAG_SIZE) &&
             tc_ccm_generation_encryption(packet + APEX_PACKET_HEADER_SIZE,
                 length + APEX_PACKET_TAG_SIZE, packet, APEX_PACKET_HEADER_SIZE,
                 payload, length, &ccm);
    return ok ? (int)(APEX_PACKET_HEADER_SIZE + length + APEX_PACKET_TAG_SIZE)
              : APEX_PACKET_AUTH_FAILED;
}

int apex_packet_decode(struct apex_session *s, const uint8_t *packet, size_t length,
                       uint8_t *type, uint8_t *payload, size_t capacity)
{
    uint8_t plain[APEX_PACKET_PAYLOAD_MAX], nonce[13];
    struct tc_ccm_mode_struct ccm;
    if (!s || !s->ready || !packet || !type || !payload ||
        length < APEX_PACKET_HEADER_SIZE + APEX_PACKET_TAG_SIZE ||
        length > APEX_PACKET_MAX || packet[0] != APEX_PROTOCOL_VERSION ||
        !valid_type(packet[1]) || packet[2] != (s->role ^ 1) ||
        packet[3] > capacity || packet[3] > APEX_PACKET_PAYLOAD_MAX ||
        length != APEX_PACKET_HEADER_SIZE + packet[3] + APEX_PACKET_TAG_SIZE ||
        memcmp(packet + 8, s->id, sizeof(s->id))) {
        return APEX_PACKET_INVALID;
    }
    uint32_t counter = read32(packet + 4);
    if (!counter || counter == UINT32_MAX) return APEX_PACKET_INVALID;
    make_nonce(nonce, packet);
    int ok = tc_ccm_config(&ccm, &s->aes, nonce, sizeof(nonce), APEX_PACKET_TAG_SIZE) &&
             tc_ccm_decryption_verification(plain, sizeof(plain),
                 packet, APEX_PACKET_HEADER_SIZE, packet + APEX_PACKET_HEADER_SIZE,
                 packet[3] + APEX_PACKET_TAG_SIZE, &ccm);
    if (!ok) {
        wipe(plain, sizeof(plain));
        return APEX_PACKET_AUTH_FAILED;
    }
    if (counter > s->rx_highest) {
        uint32_t gap = counter - s->rx_highest;
        s->rx_seen = (gap >= 64 ? 0 : s->rx_seen << gap) | 1;
        s->rx_highest = counter;
    } else {
        uint32_t gap = s->rx_highest - counter;
        if (gap >= 64 || (s->rx_seen & (UINT64_C(1) << gap))) {
            wipe(plain, sizeof(plain));
            return APEX_PACKET_REPLAY;
        }
        s->rx_seen |= UINT64_C(1) << gap;
    }
    memcpy(payload, plain, packet[3]);
    *type = packet[1];
    wipe(plain, sizeof(plain));
    return packet[3];
}
