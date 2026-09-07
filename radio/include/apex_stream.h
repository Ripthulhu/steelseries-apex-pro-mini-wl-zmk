/* SPDX-License-Identifier: MIT */
#ifndef APEX_STREAM_H
#define APEX_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APEX_STREAM_DATA_MAX 47
#define APEX_STREAM_HEADER_SIZE 6
#define APEX_STREAM_PACKET_MAX (APEX_STREAM_HEADER_SIZE + APEX_STREAM_DATA_MAX)
struct apex_stream {
    uint16_t tx_sequence, rx_sequence;
    uint8_t tx_length, rx_length;
    uint8_t tx[APEX_STREAM_DATA_MAX], rx[APEX_STREAM_DATA_MAX];
};
void apex_stream_reset(struct apex_stream *stream);
int apex_stream_put(struct apex_stream *stream, const uint8_t *data, size_t length);
int apex_stream_get(struct apex_stream *stream, uint8_t *data);
int apex_stream_pack(const struct apex_stream *stream, uint8_t *packet);
int apex_stream_receive(struct apex_stream *stream, const uint8_t *packet, size_t length);
#endif
