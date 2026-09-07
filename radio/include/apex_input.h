/* SPDX-License-Identifier: MIT */
#ifndef APEX_INPUT_H
#define APEX_INPUT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define APEX_INPUT_VERSION 2
#define APEX_INPUT_KEYBOARD 1
#define APEX_INPUT_CONSUMER 2
#define APEX_INPUT_KEYBOARD_SIZE 8
#define APEX_INPUT_CONSUMER_SIZE 12
#define APEX_INPUT_QUEUE_SIZE 32
struct apex_input_frame {
    uint32_t sequence;
    uint8_t type;
    uint8_t data[APEX_INPUT_CONSUMER_SIZE];
};
struct apex_input_queue {
    struct apex_input_frame frames[APEX_INPUT_QUEUE_SIZE];
    uint8_t keyboard[APEX_INPUT_KEYBOARD_SIZE], consumer[APEX_INPUT_CONSUMER_SIZE];
    uint32_t next_sequence;
    uint8_t head, count;
    bool online;
};
size_t apex_input_size(uint8_t type);
void apex_input_disconnect(struct apex_input_queue *q);
void apex_input_session(struct apex_input_queue *q);
int apex_input_push(struct apex_input_queue *q, uint8_t type, const uint8_t *data, size_t length);
bool apex_input_peek(const struct apex_input_queue *q, struct apex_input_frame *out);
bool apex_input_ack(struct apex_input_queue *q, uint32_t sequence);
int apex_input_pack(const struct apex_input_frame *f, uint8_t *out, size_t capacity);
int apex_input_unpack(const uint8_t *data, size_t length, struct apex_input_frame *out);
#endif
