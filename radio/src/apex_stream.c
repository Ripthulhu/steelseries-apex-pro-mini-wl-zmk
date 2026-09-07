/* SPDX-License-Identifier: MIT */
#include "apex_stream.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void put16(uint8_t *p, uint16_t value) { p[0] = value; p[1] = value >> 8; }

void apex_stream_reset(struct apex_stream *s) { memset(s, 0, sizeof(*s)); }

int apex_stream_put(struct apex_stream *s, const uint8_t *data, size_t length)
{
    if (!length || length > APEX_STREAM_DATA_MAX || s->tx_sequence == UINT16_MAX) return -1;
    if (s->tx_length) return 0;
    memcpy(s->tx, data, length);
    s->tx_length = length;
    s->tx_sequence++;
    return length;
}

int apex_stream_get(struct apex_stream *s, uint8_t *data)
{
    int length = s->rx_length;
    memcpy(data, s->rx, length);
    s->rx_length = 0;
    return length;
}

int apex_stream_pack(const struct apex_stream *s, uint8_t *packet)
{
    packet[0] = 1;
    packet[1] = s->tx_length;
    put16(packet + 2, s->tx_length ? s->tx_sequence : 0);
    put16(packet + 4, s->rx_sequence);
    memcpy(packet + APEX_STREAM_HEADER_SIZE, s->tx, s->tx_length);
    return APEX_STREAM_HEADER_SIZE + s->tx_length;
}

int apex_stream_receive(struct apex_stream *s, const uint8_t *packet, size_t length)
{
    if (length < APEX_STREAM_HEADER_SIZE || packet[0] != 1 ||
        packet[1] > APEX_STREAM_DATA_MAX || length != APEX_STREAM_HEADER_SIZE + packet[1]) return -1;
    uint16_t sequence = get16(packet + 2), ack = get16(packet + 4);
    if ((packet[1] != 0) != (sequence != 0) || ack > s->tx_sequence ||
        (uint32_t)sequence > (uint32_t)s->rx_sequence + 1) return -1;
    if (ack == s->tx_sequence) s->tx_length = 0;
    if (!sequence || sequence <= s->rx_sequence || s->rx_length) return 0;
    memcpy(s->rx, packet + APEX_STREAM_HEADER_SIZE, packet[1]);
    s->rx_length = packet[1];
    s->rx_sequence = sequence;
    return s->rx_length;
}
