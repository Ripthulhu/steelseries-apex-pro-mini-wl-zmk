/* SPDX-License-Identifier: MIT */
#include "apex_delivery.h"
#include <string.h>

void apex_delivery_rx_reset(struct apex_delivery_rx *rx)
{
    memset(rx, 0, sizeof(*rx));
}

int apex_delivery_receive(struct apex_delivery_rx *rx, const struct apex_input_frame *frame)
{
    if (!frame->sequence || !apex_input_size(frame->type)) return -1;
    if (frame->sequence <= rx->accepted) return 1;
    if (rx->accepted == UINT32_MAX || frame->sequence != rx->accepted + 1) return -1;
    if (rx->count == APEX_DELIVERY_CAPACITY) return -2;
    rx->frames[(rx->head + rx->count) % APEX_DELIVERY_CAPACITY] = *frame;
    rx->accepted = frame->sequence;
    rx->count++;
    return 0;
}

bool apex_delivery_front(const struct apex_delivery_rx *rx, struct apex_input_frame *frame)
{
    if (!rx->count) return false;
    *frame = rx->frames[rx->head];
    return true;
}

bool apex_delivery_complete(struct apex_delivery_rx *rx, uint32_t sequence)
{
    if (!rx->count || rx->frames[rx->head].sequence != sequence) return false;
    rx->completed = sequence;
    rx->head = (rx->head + 1) % APEX_DELIVERY_CAPACITY;
    rx->count--;
    return true;
}

void apex_delivery_status(const struct apex_delivery_rx *rx, uint8_t leds,
                          struct apex_delivery_ack *ack)
{
    *ack = (struct apex_delivery_ack){rx->accepted, rx->completed,
                                    APEX_DELIVERY_CAPACITY - rx->count, leds};
}

void apex_delivery_tx_reset(struct apex_delivery_tx *tx)
{
    *tx = (struct apex_delivery_tx){.free = APEX_DELIVERY_CAPACITY};
}

static bool valid_ack(const struct apex_delivery_ack *ack)
{
    return ack->completed <= ack->accepted && ack->free <= APEX_DELIVERY_CAPACITY &&
           ack->accepted - ack->completed == APEX_DELIVERY_CAPACITY - ack->free;
}

bool apex_delivery_apply(struct apex_delivery_tx *tx, const struct apex_delivery_ack *ack)
{
    if (!valid_ack(ack) || ack->accepted > tx->sent ||
        ack->accepted < tx->accepted || ack->completed < tx->completed) return false;
    tx->accepted = ack->accepted;
    tx->completed = ack->completed;
    tx->free = ack->free;
    return true;
}

bool apex_delivery_next(const struct apex_delivery_tx *tx, const struct apex_input_queue *queue,
                        struct apex_input_frame *frame)
{
    if (!tx->free || tx->accepted == UINT32_MAX) return false;
    for (unsigned int i = 0; i < queue->count; i++) {
        const struct apex_input_frame *candidate =
            &queue->frames[(queue->head + i) % APEX_INPUT_QUEUE_SIZE];
        if (candidate->sequence == tx->accepted + 1) {
            *frame = *candidate;
            return true;
        }
    }
    return false;
}

void apex_delivery_ack_pack(const struct apex_delivery_ack *ack, uint8_t *out)
{
    out[0] = APEX_DELIVERY_VERSION;
    for (unsigned int i = 0; i < 4; i++) {
        out[1 + i] = ack->accepted >> (8 * i);
        out[5 + i] = ack->completed >> (8 * i);
    }
    out[9] = ack->free;
    out[10] = ack->leds;
}

unsigned int apex_delivery_batch(const struct apex_delivery_tx *tx,
                                 const struct apex_input_queue *queue,
                                 struct apex_input_frame frames[APEX_DELIVERY_BATCH_MAX])
{
    unsigned int count = 0;
    uint32_t next = tx->accepted;
    if (!queue->online) return 0;
    for (unsigned int i = 0; i < queue->count &&
         count < APEX_DELIVERY_BATCH_MAX && next < UINT32_MAX; i++) {
        const struct apex_input_frame *f = &queue->frames[(queue->head + i) % APEX_INPUT_QUEUE_SIZE];
        if (f->sequence <= next) continue;
        if (f->sequence != next + 1) break;
        frames[count++] = *f;
        next++;
    }
    /* When USB is nearly full, let credits accumulate for the queued batch.
     * Sending one report per newly freed entry otherwise recreates stop/wait. */
    return count <= tx->free ? count : 0;
}

int apex_delivery_batch_pack(const struct apex_input_frame *frames, unsigned int count,
                             uint8_t *out, size_t capacity)
{
    if (!frames || !out || !count || count > APEX_DELIVERY_BATCH_MAX || capacity < 2) return -1;
    size_t offset = 2;
    for (unsigned int i = 0; i < count; i++) {
        if (i && (frames[i - 1].sequence == UINT32_MAX ||
                  frames[i].sequence != frames[i - 1].sequence + 1)) return -1;
        int n = apex_input_pack(&frames[i], out + offset, capacity - offset);
        if (n < 0) return -1;
        offset += n;
    }
    out[0] = APEX_DELIVERY_VERSION;
    out[1] = count;
    return offset;
}

int apex_delivery_batch_unpack(const uint8_t *data, size_t length,
                               struct apex_input_frame frames[APEX_DELIVERY_BATCH_MAX])
{
    if (!data || !frames || length < 2 || data[0] != APEX_DELIVERY_VERSION ||
        !data[1] || data[1] > APEX_DELIVERY_BATCH_MAX) return -1;
    struct apex_input_frame decoded[APEX_DELIVERY_BATCH_MAX];
    size_t offset = 2;
    for (unsigned int i = 0; i < data[1]; i++) {
        if (length - offset < 6) return -1;
        size_t n = 6 + apex_input_size(data[offset + 5]);
        if (n > length - offset || apex_input_unpack(data + offset, n, &decoded[i])) return -1;
        if (i && (decoded[i - 1].sequence == UINT32_MAX ||
                  decoded[i].sequence != decoded[i - 1].sequence + 1)) return -1;
        offset += n;
    }
    if (offset != length) return -1;
    memcpy(frames, decoded, data[1] * sizeof(*frames));
    return data[1];
}

bool apex_delivery_ack_unpack(const uint8_t *data, size_t length, struct apex_delivery_ack *ack)
{
    if (!data || !ack || length != APEX_DELIVERY_ACK_SIZE || data[0] != APEX_DELIVERY_VERSION)
        return false;
    struct apex_delivery_ack decoded = {.free = data[9], .leds = data[10]};
    for (unsigned int i = 0; i < 4; i++) {
        decoded.accepted |= (uint32_t)data[1 + i] << (8 * i);
        decoded.completed |= (uint32_t)data[5 + i] << (8 * i);
    }
    if (!valid_ack(&decoded)) return false;
    *ack = decoded;
    return true;
}
