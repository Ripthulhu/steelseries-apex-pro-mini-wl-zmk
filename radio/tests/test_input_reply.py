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
    handler = source[start:source.index('\n#endif', start)]
    harness = r'''
#include "apex_input.h"
#include <assert.h>
#include <errno.h>
#define APEX_KEYBOARD 0
#define APEX_PACKET_ACK 1
#define APEX_PACKET_INPUT 2
#define APEX_PACKET_KEEPALIVE 3
typedef int k_spinlock_key_t;
static int local_role = 1, input_lock, input_acked, usb_waits, input_delivered;
static int duplicate_reports, delivery_result = -EAGAIN;
static uint32_t pending_usb_sequence, rx_sequence, last_ack;
static struct apex_input_queue input_queue;
#define atomic_get(p) (*(p))
#define atomic_set(p,v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int key) { (void)p; (void)key; }
static uint32_t sys_get_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void apex_radio_update_leds(uint8_t value) { (void)value; }
static int apex_radio_deliver(const struct apex_input_frame *f) {
    (void)f; return delivery_result;
}
static int input_ack(uint32_t sequence) { last_ack = sequence; return 6; }
'''
    checks = r'''
int main(void) {
    struct apex_input_frame f = {.sequence = 1, .type = APEX_INPUT_KEYBOARD};
    uint8_t data[18];
    int n = apex_input_pack(&f, data, sizeof(data));
    assert(n > 0);
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 0);
    assert(rx_sequence == 0 && pending_usb_sequence == 1);
    /* A stalled USB endpoint still gets liveness replies on retries. */
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 6 && last_ack == 0);
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 6 && last_ack == 0);
    delivery_result = 0;
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 6 && last_ack == 1);
    assert(rx_sequence == 1 && input_delivered == 1);
    /* Lost completion ACKs are recovered without another USB submission. */
    delivery_result = -EIO;
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 6 && last_ack == 1);
    assert(duplicate_reports == 1 && input_delivered == 1);
    f.sequence = 2; n = apex_input_pack(&f, data, sizeof(data));
    assert(input_packet(APEX_PACKET_INPUT, data, n) == -EIO);
    assert(rx_sequence == 1);
    delivery_result = -EAGAIN;
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 0);
    assert(input_packet(APEX_PACKET_INPUT, data, n) == 6 && last_ack == 1);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-reply-test-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + handler + checks)
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root / 'include'), str(path / 'test.c'),
                        str(root / 'src/apex_input.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Input reply tests passed')


if __name__ == '__main__':
    main()
