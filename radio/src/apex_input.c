/* SPDX-License-Identifier: MIT */
#include "apex_input.h"
#include <string.h>

size_t apex_input_size(uint8_t type)
{
    return type == APEX_INPUT_KEYBOARD ? APEX_INPUT_KEYBOARD_SIZE :
           type == APEX_INPUT_CONSUMER ? APEX_INPUT_CONSUMER_SIZE : 0;
}

static int enqueue(struct apex_input_queue *q, uint8_t type, const uint8_t *data)
{
    if (q->count == APEX_INPUT_QUEUE_SIZE || q->next_sequence == UINT32_MAX) return -2;
    struct apex_input_frame *f = &q->frames[(q->head + q->count) % APEX_INPUT_QUEUE_SIZE];
    memset(f, 0, sizeof(*f));
    f->sequence = q->next_sequence++;
    f->type = type;
    memcpy(f->data, data, apex_input_size(type));
    q->count++;
    return 0;
}

void apex_input_disconnect(struct apex_input_queue *q)
{
    q->head = q->count = 0;
    q->online = false;
}

void apex_input_session(struct apex_input_queue *q)
{
    apex_input_disconnect(q);
    q->online = true;
    q->next_sequence = 1;
    enqueue(q, APEX_INPUT_KEYBOARD, q->keyboard);
    enqueue(q, APEX_INPUT_CONSUMER, q->consumer);
}

int apex_input_push(struct apex_input_queue *q, uint8_t type, const uint8_t *data, size_t length)
{
    size_t size = apex_input_size(type);
    if (!q || !data || !size || length != size) return -1;
    uint8_t *latest = type == APEX_INPUT_KEYBOARD ? q->keyboard : q->consumer;
    if (!memcmp(latest, data, size)) return 0;
    memcpy(latest, data, size);
    return q->online ? enqueue(q, type, data) : 0;
}

bool apex_input_peek(const struct apex_input_queue *q, struct apex_input_frame *out)
{
    if (!q->count) return false;
    *out = q->frames[q->head];
    return true;
}

bool apex_input_ack(struct apex_input_queue *q, uint32_t sequence)
{
    if (!q->count || q->frames[q->head].sequence != sequence) return false;
    q->head = (q->head + 1) % APEX_INPUT_QUEUE_SIZE;
    q->count--;
    return true;
}

int apex_input_pack(const struct apex_input_frame *f, uint8_t *out, size_t capacity)
{
    size_t n = apex_input_size(f->type);
    if (!n || !f->sequence || capacity < n + 6) return -1;
    out[0] = APEX_INPUT_VERSION;
    for (int i = 0; i < 4; i++) out[1 + i] = f->sequence >> (i * 8);
    out[5] = f->type;
    memcpy(out + 6, f->data, n);
    return n + 6;
}

int apex_input_unpack(const uint8_t *data, size_t length, struct apex_input_frame *out)
{
    if (!data || !out || length < 6 || data[0] != APEX_INPUT_VERSION) return -1;
    size_t n = apex_input_size(data[5]);
    if (!n || length != n + 6) return -1;
    uint32_t sequence = 0;
    for (int i = 0; i < 4; i++) sequence |= (uint32_t)data[1 + i] << (i * 8);
    if (!sequence) return -1;
    memset(out, 0, sizeof(*out));
    out->sequence = sequence;
    out->type = data[5];
    memcpy(out->data, data + 6, n);
    return 0;
}
