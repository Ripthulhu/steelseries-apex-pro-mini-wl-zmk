#!/usr/bin/env python3
"""Test receiver reply policy with the actual input handler and wire format."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/apex_radio_probe.c').read_text()
    start = source.index('static int input_packet(')
    # Capture the whole function (it now contains nested #if IS_ENABLED blocks),
    # up to the closing brace that is followed by its guard's #endif.
    end = source.index('\n}\n#endif', start)
    handler = source[start:end] + '\n}\n'
    harness = r'''
#include "apex_input.h"
#include "apex_delivery.h"
#include "apex_latency.h"
#include <assert.h>
#include <errno.h>
#define APEX_KEYBOARD 0
#define APEX_PACKET_ACK 1
#define APEX_PACKET_INPUT 2
#define APEX_PACKET_KEEPALIVE 3
#define APEX_PACKET_INPUT_BATCH 6
#define APEX_PACKET_PAYLOAD_MAX 64
#define BUILD_ASSERT(x) _Static_assert(x, #x)
/* Compile out the CONFIG_APEX_RADIO_SHELL branches; this test exercises the
   input/ack/batch policy, not the shell relay. */
#define IS_ENABLED(x) 0
typedef int k_spinlock_key_t;
static int local_role = 1, input_lock, input_acked, usb_waits;
static int duplicate_reports, delivery_result = -EAGAIN, delivered_count, full_after = -1;
static int input_progress, reply_pending;
static uint32_t rx_sequence, last_ack;
static struct apex_delivery_tx delivery_tx;
static struct apex_input_queue input_queue;
static struct apex_latency queue_latency;
static uint32_t queued_at[APEX_INPUT_QUEUE_SIZE];
static uint32_t k_cycle_get_32(void) { return 100; }
static uint32_t k_cyc_to_us_floor32(uint32_t cycles) { return cycles; }
static void input_trace_acked(uint32_t sequence, bool more) { (void)sequence; (void)more; }
#define atomic_get(p) (*(p))
#define atomic_set(p,v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int key) { (void)p; (void)key; }
static void apex_radio_update_leds(uint8_t value) { (void)value; }
static int apex_radio_deliver(const struct apex_input_frame *f) {
    (void)f;
    if (full_after >= 0 && delivered_count >= full_after) return -EAGAIN;
    if (!delivery_result) delivered_count++;
    return delivery_result;
}
static int input_ack(uint32_t sequence) { last_ack = sequence; return 6; }
#define APEX_PACKET_GAMEPAD 7
static uint32_t gamepad_rx_counter;
static bool apex_gamepad_valid(const uint8_t *p, size_t l) { (void)p; (void)l; return false; }
static bool apex_gamepad_newer(uint32_t c, uint32_t *last) { (void)c; (void)last; return false; }
static void apex_radio_gamepad_receive(bool e, const uint8_t *r) { (void)e; (void)r; }
'''
    checks = r'''
int main(void) {
    struct apex_input_frame f = {.sequence = 1, .type = APEX_INPUT_KEYBOARD};
    uint8_t data[18];
    int n = apex_input_pack(&f, data, sizeof(data));
    assert(n > 0);
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6);
    assert(rx_sequence == 0);
    /* A full receiver still sends its current acceptance position. */
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6 && last_ack == 0);
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6 && last_ack == 0);
    delivery_result = 0;
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6 && last_ack == 1);
    assert(rx_sequence == 1);
    /* Lost acceptance ACKs do not enqueue the report twice. */
    delivery_result = -EIO;
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6 && last_ack == 1);
    assert(duplicate_reports == 1);
    f.sequence = 2; n = apex_input_pack(&f, data, sizeof(data));
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == -EIO);
    assert(rx_sequence == 1);
    delivery_result = -EAGAIN;
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6);
    assert(input_packet(APEX_PACKET_INPUT, data, n, 0) == 6 && last_ack == 1);
    struct apex_input_frame batch[3] = {{.sequence=2,.type=APEX_INPUT_KEYBOARD},
        {.sequence=3,.type=APEX_INPUT_CONSUMER}, {.sequence=4,.type=APEX_INPUT_KEYBOARD}};
    uint8_t packed[APEX_DELIVERY_BATCH_SIZE];
    int bytes = apex_delivery_batch_pack(batch, 3, packed, sizeof(packed));
    delivered_count = 0; delivery_result = 0; full_after = 2;
    assert(input_packet(APEX_PACKET_INPUT_BATCH, packed, bytes, 0) == 6);
    assert(rx_sequence == 3 && delivered_count == 2 && last_ack == 3);
    full_after = -1;
    assert(input_packet(APEX_PACKET_INPUT_BATCH, packed, bytes, 0) == 6);
    assert(rx_sequence == 4 && delivered_count == 3 && last_ack == 4);
    assert(input_packet(APEX_PACKET_INPUT_BATCH, packed, bytes, 0) == 6);
    assert(delivered_count == 3);
    for (int len = 0; len < bytes; len++)
        assert(input_packet(APEX_PACKET_INPUT_BATCH, packed, len, 0) == -EINVAL);
    assert(delivered_count == 3);
    local_role = APEX_KEYBOARD;
    apex_input_session(&input_queue);
    apex_delivery_tx_reset(&delivery_tx);
    delivery_tx.sent = 2;
    uint8_t ack[APEX_DELIVERY_ACK_SIZE];
    struct apex_delivery_ack status = {1, 0, 7, 0};
    apex_delivery_ack_pack(&status, ack);
    assert(input_packet(APEX_PACKET_ACK, ack, sizeof(ack), 0) == 0);
    assert(input_progress == 1 && input_acked == 0 && input_queue.count == 2);
    status.completed = 1; status.free = 8;
    apex_delivery_ack_pack(&status, ack);
    assert(input_packet(APEX_PACKET_ACK, ack, sizeof(ack), 0) == 0);
    assert(input_progress == 1 && input_acked == 1 && input_queue.count == 1);
    assert(queue_latency.count == 1 && queue_latency.total_us == 100);
    input_progress = 0;
    assert(input_packet(APEX_PACKET_ACK, ack, sizeof(ack), 0) == 0);
    assert(input_progress == 0 && input_acked == 1 && input_queue.count == 1);
    assert(queue_latency.count == 1);
    queued_at[input_queue.head] = UINT32_MAX - 49;
    status.accepted = status.completed = 2;
    apex_delivery_ack_pack(&status, ack);
    assert(input_packet(APEX_PACKET_ACK, ack, sizeof(ack), 0) == 0);
    assert(queue_latency.count == 2 && queue_latency.total_us == 250);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-reply-test-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + handler + checks)
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root / 'include'), str(path / 'test.c'),
                        str(root / 'src/apex_input.c'), str(root / 'src/apex_delivery.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Input reply tests passed')


if __name__ == '__main__':
    main()
