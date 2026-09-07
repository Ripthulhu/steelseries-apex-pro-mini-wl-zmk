#!/usr/bin/env python3
"""Test report timing across retries, idle queues and cycle-counter wrap."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
functions = ''
for name in ('input_trace_sent', 'input_trace_acked'):
    start = source.index('static void ' + name + '(')
    functions += source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include "apex_latency.h"
#include <stdbool.h>
#include <assert.h>
static struct apex_latency send_ack_latency, ack_next_latency;
static uint32_t timed_sequence, first_send_at, last_ack_at, clock_now;
static bool next_after_ack;
static uint32_t k_cycle_get_32(void) { return clock_now; }
static uint32_t k_cyc_to_us_floor32(uint32_t value) { return value; }
'''
checks = r'''
int main(void) {
    input_trace_sent(1, 100);
    input_trace_sent(1, 200);
    clock_now=300;
    input_trace_acked(1, true);
    assert(send_ack_latency.count==1 && send_ack_latency.total_us==200);
    input_trace_sent(2, 350);
    assert(ack_next_latency.count==1 && ack_next_latency.total_us==50);
    input_trace_sent(2, 400);
    assert(ack_next_latency.count==1);
    clock_now=500;
    input_trace_acked(2, false);
    input_trace_sent(3, UINT32_MAX-49);
    assert(ack_next_latency.count==1);
    clock_now=100;
    input_trace_acked(3, true);
    assert(send_ack_latency.count==3 && send_ack_latency.total_us==500);
    next_after_ack=false; timed_sequence=0;
    input_trace_sent(1, 1000);
    assert(ack_next_latency.count==1 && first_send_at==1000);
}
'''
with tempfile.TemporaryDirectory(prefix='apex-delivery-timing-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + functions + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Delivery timing tests passed')
