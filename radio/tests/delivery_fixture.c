/* SPDX-License-Identifier: MIT */
#include "apex_delivery.h"
#include <assert.h>
#include <string.h>

static void lossy_link(void)
{
    struct apex_delivery_rx rx = {0};
    struct apex_delivery_tx tx;
    struct apex_input_queue queue = {0};
    struct apex_input_frame frame;
    struct apex_delivery_ack ack, delayed = {0};
    uint32_t generated = 0, usb = 0;
    bool have_delayed = false;
    apex_delivery_tx_reset(&tx);
    for (unsigned int tick = 0; tick < 100000 && usb < 1000; tick++) {
        if (generated < 1000 && queue.count < APEX_INPUT_QUEUE_SIZE) {
            frame = (struct apex_input_frame){.sequence = ++generated,
                .type = generated & 1 ? APEX_INPUT_KEYBOARD : APEX_INPUT_CONSUMER};
            frame.data[0] = generated;
            queue.frames[(queue.head + queue.count++) % APEX_INPUT_QUEUE_SIZE] = frame;
        }
        if (apex_delivery_next(&tx, &queue, &frame)) {
            tx.sent = frame.sequence;
            if (tick % 7) {
                int rc = apex_delivery_receive(&rx, &frame);
                assert(rc == 0 || rc == 1 || rc == -2);
            }
        }
        /* Stall USB long enough to fill the FIFO, then consume intermittently. */
        if (tick > 100 && tick % 3 == 0 && apex_delivery_front(&rx, &frame)) {
            assert(frame.sequence == usb + 1 && frame.data[0] == (uint8_t)(usb + 1));
            assert(apex_delivery_complete(&rx, frame.sequence));
            usb++;
        }
        apex_delivery_status(&rx, 0, &ack);
        if (tick % 5 && apex_delivery_apply(&tx, &ack)) {
            while (queue.count && queue.frames[queue.head].sequence <= tx.completed)
                assert(apex_input_ack(&queue, queue.frames[queue.head].sequence));
        }
        if (have_delayed) {
            uint32_t accepted = tx.accepted, completed = tx.completed;
            (void)apex_delivery_apply(&tx, &delayed);
            assert(tx.accepted >= accepted && tx.completed >= completed);
        }
        if (tick % 11 == 0) { delayed = ack; have_delayed = true; }
        assert(rx.count <= APEX_DELIVERY_CAPACITY && queue.count <= APEX_INPUT_QUEUE_SIZE);
    }
    assert(usb == 1000 && rx.completed == 1000);
}

static void batched_link(void)
{
    struct apex_delivery_rx rx = {0};
    struct apex_delivery_tx tx;
    struct apex_input_queue queue = {.online = true};
    struct apex_input_frame frames[APEX_DELIVERY_BATCH_MAX], decoded[APEX_DELIVERY_BATCH_MAX];
    uint8_t wire[APEX_DELIVERY_BATCH_SIZE];
    uint32_t generated = 0, completed = 0;
    apex_delivery_tx_reset(&tx);
    for (unsigned int tick = 0; tick < 50000 && completed < 1000; tick++) {
        while (generated < 1000 && queue.count < APEX_INPUT_QUEUE_SIZE) {
            struct apex_input_frame f = {.sequence = ++generated,
                .type = generated & 1 ? APEX_INPUT_KEYBOARD : APEX_INPUT_CONSUMER};
            f.data[0] = generated;
            queue.frames[(queue.head + queue.count++) % APEX_INPUT_QUEUE_SIZE] = f;
        }
        unsigned int count = apex_delivery_batch(&tx, &queue, frames);
        assert(count <= tx.free && count <= APEX_DELIVERY_BATCH_MAX);
        if (count) {
            uint32_t last = frames[count - 1].sequence;
            if (last > tx.sent) tx.sent = last;
            int n = apex_delivery_batch_pack(frames, count, wire, sizeof(wire));
            assert(n > 0 && n <= 64);
            assert(apex_delivery_batch_unpack(wire, n, decoded) == (int)count);
            if (tick % 7) {
                for (unsigned int i = 0; i < count; i++) {
                    int rc = apex_delivery_receive(&rx, &decoded[i]);
                    assert(rc == 0 || rc == 1 || rc == -2);
                    if (rc == -2) break;
                }
            }
        }
        if (tick > 100 && tick % 3 == 0 && apex_delivery_front(&rx, frames)) {
            assert(frames[0].sequence == completed + 1);
            assert(frames[0].data[0] == (uint8_t)(completed + 1));
            assert(apex_delivery_complete(&rx, ++completed));
        }
        struct apex_delivery_ack ack;
        apex_delivery_status(&rx, 3, &ack);
        if (tick % 5 && apex_delivery_apply(&tx, &ack)) {
            while (queue.count && queue.frames[queue.head].sequence <= tx.completed)
                assert(apex_input_ack(&queue, queue.frames[queue.head].sequence));
        }
        assert(rx.count <= APEX_DELIVERY_CAPACITY);
    }
    assert(completed == 1000);
    frames[0] = (struct apex_input_frame){.sequence=1,.type=APEX_INPUT_KEYBOARD};
    frames[1] = (struct apex_input_frame){.sequence=2,.type=APEX_INPUT_CONSUMER};
    frames[2] = (struct apex_input_frame){.sequence=3,.type=APEX_INPUT_KEYBOARD};
    int n = apex_delivery_batch_pack(frames, 3, wire, sizeof(wire));
    memset(decoded, 0xa5, sizeof(decoded));
    struct apex_input_frame saved[APEX_DELIVERY_BATCH_MAX];
    memcpy(saved, decoded, sizeof(saved));
    for (int size = 0; size < n; size++) {
        assert(apex_delivery_batch_unpack(wire, size, decoded) == -1);
        assert(!memcmp(saved, decoded, sizeof(saved)));
        assert(apex_delivery_batch_pack(frames, 3, wire, size) == -1);
    }
    assert(apex_delivery_batch_pack(frames, 3, wire, sizeof(wire)) == n);
    wire[1] = 0; assert(apex_delivery_batch_unpack(wire, n, decoded) == -1);
    wire[1] = 4; assert(apex_delivery_batch_unpack(wire, n, decoded) == -1);
    wire[1] = 3; wire[17] = 8; /* Second report's sequence no longer follows first. */
    assert(apex_delivery_batch_unpack(wire, n, decoded) == -1);
    frames[1].sequence = 5;
    assert(apex_delivery_batch_pack(frames, 3, wire, sizeof(wire)) == -1);
    frames[0].sequence = UINT32_MAX; frames[1].sequence = 0;
    assert(apex_delivery_batch_pack(frames, 2, wire, sizeof(wire)) == -1);
    tx.accepted = UINT32_MAX; assert(!apex_delivery_batch(&tx, &queue, frames));
}

int main(void)
{
    lossy_link();
    batched_link();
    struct apex_delivery_rx rx;
    struct apex_delivery_tx tx;
    struct apex_delivery_ack ack, decoded;
    struct apex_input_queue queue = {0};
    struct apex_input_frame frame = {.type = APEX_INPUT_KEYBOARD}, front;
    uint8_t wire[APEX_DELIVERY_ACK_SIZE];
    apex_delivery_rx_reset(&rx);
    apex_delivery_tx_reset(&tx);
    apex_delivery_status(&rx, 3, &ack);
    assert(apex_delivery_apply(&tx, &ack));
    assert(tx.free == 8);

    /* USB is stopped. Fill the receiver without completing any reports. */
    for (uint32_t i = 1; i <= 8; i++) {
        frame.sequence = i;
        frame.data[2] = (i & 1) ? 4 : 0;
        queue.frames[queue.count++] = frame;
        assert(apex_delivery_next(&tx, &queue, &front));
        assert(front.sequence == i);
        assert(apex_delivery_receive(&rx, &frame) == 0);
        tx.sent = i;
        assert(apex_delivery_receive(&rx, &frame) == 1);
        apex_delivery_status(&rx, 3, &ack);
        assert(apex_delivery_apply(&tx, &ack));
        assert(tx.completed == 0 && tx.accepted == i);
    }
    frame.sequence = 9;
    assert(apex_delivery_receive(&rx, &frame) == -2);
    assert(!apex_delivery_next(&tx, &queue, &front));
    assert(!apex_delivery_complete(&rx, 2));
    assert(rx.count == 8);

    /* Complete the reports in order, checking each press/release survived. */
    for (uint32_t i = 1; i <= 8; i++) {
        assert(apex_delivery_front(&rx, &front));
        assert(front.sequence == i && front.data[2] == ((i & 1) ? 4 : 0));
        assert(apex_delivery_complete(&rx, i));
        assert(!apex_delivery_complete(&rx, i));
    }
    assert(!apex_delivery_front(&rx, &front));
    apex_delivery_status(&rx, 3, &ack);
    apex_delivery_ack_pack(&ack, wire);
    assert(apex_delivery_ack_unpack(wire, sizeof(wire), &decoded));
    assert(decoded.accepted == 8 && decoded.completed == 8 && decoded.leds == 3);
    assert(apex_delivery_apply(&tx, &decoded));
    assert(tx.free == 8);

    /* Delayed, impossible and old-format ACKs cannot change the sender. */
    struct apex_delivery_tx saved = tx;
    ack = (struct apex_delivery_ack){9, 9, 8, 0};
    assert(!apex_delivery_apply(&tx, &ack));
    ack = (struct apex_delivery_ack){7, 7, 8, 0};
    assert(!apex_delivery_apply(&tx, &ack));
    ack = (struct apex_delivery_ack){8, 9, 8, 0};
    assert(!apex_delivery_apply(&tx, &ack));
    ack = (struct apex_delivery_ack){8, 8, 7, 0};
    assert(!apex_delivery_apply(&tx, &ack));
    assert(!memcmp(&saved, &tx, sizeof(tx)));
    wire[0] = 1;
    assert(!apex_delivery_ack_unpack(wire, sizeof(wire), &decoded));
    wire[0] = APEX_DELIVERY_VERSION;
    for (size_t n = 0; n < sizeof(wire); n++)
        assert(!apex_delivery_ack_unpack(wire, n, &decoded));
    wire[9] = 9;
    assert(!apex_delivery_ack_unpack(wire, sizeof(wire), &decoded));

    /* Exercise ring wrap with duplicates and gaps over many USB completions. */
    for (uint32_t i = 9; i < 10000; i++) {
        frame.sequence = i + 1;
        assert(apex_delivery_receive(&rx, &frame) == -1);
        frame.sequence = i;
        assert(apex_delivery_receive(&rx, &frame) == 0);
        assert(apex_delivery_receive(&rx, &frame) == 1);
        assert(apex_delivery_complete(&rx, i));
    }
    apex_delivery_rx_reset(&rx);
    apex_delivery_tx_reset(&tx);
    assert(rx.count == 0 && rx.accepted == 0 && rx.completed == 0);
    assert(tx.sent == 0 && tx.accepted == 0 && tx.completed == 0);
    assert(!apex_delivery_complete(&rx, 9999));
    frame.sequence = 0;
    assert(apex_delivery_receive(&rx, &frame) == -1);
    rx.accepted = rx.completed = UINT32_MAX;
    frame.sequence = 1;
    assert(apex_delivery_receive(&rx, &frame) == 1);
    tx.accepted = UINT32_MAX;
    assert(!apex_delivery_next(&tx, &queue, &front));
    return 0;
}
