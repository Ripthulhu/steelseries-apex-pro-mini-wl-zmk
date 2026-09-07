#!/usr/bin/env python3
"""Check that new input wakes an idle transaction without interrupting a reply."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
start = source.index('int apex_radio_queue_report(')
function = source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include "apex_delivery.h"
#include <assert.h>
#include <errno.h>
typedef int k_spinlock_key_t;
#define atomic_get(p) (*(p))
#define atomic_set(p,v) (*(p)=(v))
#define atomic_inc(p) (++*(p))
#define atomic_or(p,v) (*(p)|=(v))
#define RESET_QUEUE 1
static int input_selected=1, input_lock, reply_pending, queue_overflows, resync;
static int queue_high_water, input_progress, wakes;
static uint32_t queued_at[APEX_INPUT_QUEUE_SIZE];
static struct apex_input_queue input_queue;
static struct apex_delivery_tx delivery_tx;
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p,int key) { (void)p; (void)key; }
static uint32_t k_cycle_get_32(void) { return 100; }
static void maximum(int *p, int value) { if (value>*p) *p=value; }
static void apex_radio_delivery_notify(void) { wakes++; }
'''
checks = r'''
int main(void) {
    apex_input_session(&input_queue);
    delivery_tx=(struct apex_delivery_tx){.sent=2,.accepted=2,.free=6};
    uint8_t keys[8]={1};
    assert(!apex_radio_queue_report(1,keys,8));
    assert(wakes==1 && input_progress && input_queue.count==3);
    assert(queued_at[2]==100); /* Accepted reports still occupy the older entries. */
    reply_pending=1; input_progress=0; keys[0]++;
    assert(!apex_radio_queue_report(1,keys,8) && wakes==1 && !input_progress);
    reply_pending=0; delivery_tx.sent=4; keys[0]++;
    assert(!apex_radio_queue_report(1,keys,8) && wakes==1);
    delivery_tx.accepted=delivery_tx.sent; delivery_tx.free=0; keys[0]++;
    assert(!apex_radio_queue_report(1,keys,8) && wakes==1);
    delivery_tx.free=8;
    assert(!apex_radio_queue_report(1,keys,8) && wakes==1); /* Unchanged state. */
    input_selected=0; keys[0]++;
    assert(!apex_radio_queue_report(1,keys,8) && wakes==1);
}
'''
with tempfile.TemporaryDirectory(prefix='apex-refill-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    str(root / 'src/apex_input.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Input refill tests passed')
