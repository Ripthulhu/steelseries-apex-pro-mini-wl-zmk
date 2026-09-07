/* SPDX-License-Identifier: MIT */
#ifndef APEX_DELIVERY_H
#define APEX_DELIVERY_H
#include "apex_input.h"

#define APEX_DELIVERY_CAPACITY 8u
#define APEX_DELIVERY_ACK_SIZE 11
#define APEX_DELIVERY_VERSION 2
#define APEX_DELIVERY_BATCH_MAX 3u
#define APEX_DELIVERY_BATCH_SIZE (2 + APEX_DELIVERY_BATCH_MAX * (6 + APEX_INPUT_CONSUMER_SIZE))

/* Callers serialize access and authenticate ACKs before applying them. */
struct apex_delivery_ack {
    uint32_t accepted, completed;
    uint8_t free, leds;
};
struct apex_delivery_rx {
    struct apex_input_frame frames[APEX_DELIVERY_CAPACITY];
    uint32_t accepted, completed;
    uint8_t head, count;
};
struct apex_delivery_tx {
    uint32_t sent, accepted, completed;
    uint8_t free;
};

void apex_delivery_rx_reset(struct apex_delivery_rx *rx);
/* 0: queued, 1: duplicate, -1: invalid/out of order, -2: full. */
int apex_delivery_receive(struct apex_delivery_rx *rx, const struct apex_input_frame *frame);
bool apex_delivery_front(const struct apex_delivery_rx *rx, struct apex_input_frame *frame);
bool apex_delivery_complete(struct apex_delivery_rx *rx, uint32_t sequence);
void apex_delivery_status(const struct apex_delivery_rx *rx, uint8_t leds,
                          struct apex_delivery_ack *ack);
void apex_delivery_tx_reset(struct apex_delivery_tx *tx);
bool apex_delivery_apply(struct apex_delivery_tx *tx, const struct apex_delivery_ack *ack);
bool apex_delivery_next(const struct apex_delivery_tx *tx, const struct apex_input_queue *queue,
                        struct apex_input_frame *frame);
unsigned int apex_delivery_batch(const struct apex_delivery_tx *tx,
                                 const struct apex_input_queue *queue,
                                 struct apex_input_frame frames[APEX_DELIVERY_BATCH_MAX]);
int apex_delivery_batch_pack(const struct apex_input_frame *frames, unsigned int count,
                             uint8_t *out, size_t capacity);
int apex_delivery_batch_unpack(const uint8_t *data, size_t length,
                               struct apex_input_frame frames[APEX_DELIVERY_BATCH_MAX]);
void apex_delivery_ack_pack(const struct apex_delivery_ack *ack, uint8_t *out);
bool apex_delivery_ack_unpack(const uint8_t *data, size_t length, struct apex_delivery_ack *ack);
#endif
