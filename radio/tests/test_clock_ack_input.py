#!/usr/bin/env python3
"""Check the actual transport's clock-ACK scheduling guard on a host compiler."""
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
    start = source.index('static void clock_ack_input(')
    function = source[start:source.index('\n}\n', start) + 3]
    harness = r'''
#include <stdint.h>
#include <assert.h>
#define APEX_KEYBOARD 0
typedef int k_spinlock_key_t;
static int local_role, input_lock, input_progress, clock_input_advances, ready;
static struct { uint32_t acked_sync; } hop_link;
static struct { unsigned int count; } input_queue;
#define apex_hop_link_ready(l,t) ((void)(l), (void)(t), ready)
#define atomic_set(p,v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int k) { (void)p; (void)k; }
'''
    checks = r'''
int main(void) {
    for (local_role = 0; local_role < 2; local_role++)
        for (ready = 0; ready < 2; ready++)
            for (input_queue.count = 0; input_queue.count < 2; input_queue.count++)
                for (hop_link.acked_sync = 9; hop_link.acked_sync < 12; hop_link.acked_sync++) {
                    input_progress = clock_input_advances = 0;
                    clock_ack_input(10, 1234);
                    int expected = local_role == 0 && ready && input_queue.count &&
                                   hop_link.acked_sync > 10;
                    assert(input_progress == expected && clock_input_advances == expected);
                    assert(input_queue.count <= 1);
                }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-clock-ack-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + function + checks)
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Clock ACK input scheduling tests passed')


if __name__ == '__main__':
    main()
