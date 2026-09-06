#!/usr/bin/env python3
"""Exercise the receiver's actual completion callback with a host C compiler."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / 'src/hid.c').read_text()
    start = source.index('static void report_done(')
    end = source.index('\nstatic void set_protocol(', start)
    callback = source[start:end]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ARG_UNUSED(x) (void)(x)
#define APEX_INPUT_CONSUMER 2
struct device { int unused; };
typedef int k_spinlock_key_t;
static int lock;
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int k) { (void)p; (void)k; }
static uint16_t sys_get_le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static bool busy;
static uint32_t pending_generation, generation, pending_sequence, delivered_sequence;
static uint32_t transfer_errors, completed[2];
static uint8_t pending_type, release_mask, consumer[12];
static uint16_t last_media_usage;
static uint32_t submitted_at, delivered_at, usb_max_us, notifications;
static uint32_t k_cycle_get_32(void) { return 100; }
static uint32_t k_cyc_to_us_floor32(uint32_t cycles) { return cycles; }
static void apex_radio_delivery_notify(void) { notifications++; }
'''
    checks = r'''
int main(void) {
    generation = pending_generation = 3;
    pending_sequence = 12; delivered_sequence = 11;
    pending_type = 1; busy = true;
    report_done(0, 0, -5);
    assert(!busy && transfer_errors == 1 && delivered_sequence == 11);
    assert(completed[0] == 0);
    assert(notifications == 0);
    busy = true;
    report_done(0, 0, 0);
    assert(!busy && delivered_sequence == 12 && completed[0] == 1);
    assert(notifications == 1 && usb_max_us == 100);

    /* A completion from the previous session cannot advance the new one. */
    generation++; delivered_sequence = 0; release_mask = 3; busy = true;
    report_done(0, 0, 0);
    assert(!busy && delivered_sequence == 0 && release_mask == 3);
    assert(completed[0] == 1);
    assert(notifications == 1);

    /* Failed all-keys-up reports must also be retried. */
    pending_generation = generation; pending_sequence = 0; busy = true;
    report_done(0, 0, -125);
    assert(!busy && release_mask == 3 && transfer_errors == 2);
    busy = true;
    report_done(0, 0, 0);
    assert(!busy && release_mask == 2);

    pending_type = 2; busy = true;
    report_done(0, 0, 0);
    assert(release_mask == 0);
    pending_sequence = 1; consumer[0] = 0xcd; busy = true;
    report_done(0, 0, 0);
    assert(delivered_sequence == 1 && completed[1] == 1 && last_media_usage == 0xcd);
    assert(notifications == 2);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-hid-test-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + callback + checks)
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('HID completion tests passed')


if __name__ == '__main__':
    main()
