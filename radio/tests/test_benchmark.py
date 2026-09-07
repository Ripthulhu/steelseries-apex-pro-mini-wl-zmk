#!/usr/bin/env python3
"""Check the bounded benchmark and verify it never generates key presses."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
start = source.index('int apex_radio_benchmark(')
function = source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include "apex_input.h"
#include "apex_delivery.h"
#define APEX_KEYBOARD 0
#define ARG_UNUSED(x) (void)(x)
#define K_MSEC(x) (x)
#define atomic_get(p) (*(p))
static int atomic_set(int *p, int v) { int old=*p; *p=v; return old; }
#define atomic_cas(p,o,n) (*(p)==(o) ? (*(p)=(n), 1) : 0)
static int usb_ready, connected;
#define zmk_usb_is_hid_ready() usb_ready
#define apex_radio_input_connected() connected
static int benchmark_running;
static int reply_pending;
static struct apex_delivery_tx delivery_tx = {.free = 8};
static int clock_tx_lead_us=2000;
struct shell { int unused; };
typedef int k_spinlock_key_t;
static struct apex_input_queue input_queue;
static int input_lock, local_role, input_selected, resync, sessions, input_acked;
static int input_progress, queue_high_water, requested, locked, scenario;
static uint32_t queued_at[APEX_INPUT_QUEUE_SIZE], ticks, received;
static int k_spin_lock(int *p) { (void)p; assert(!locked); locked=1; return 0; }
static void k_spin_unlock(int *p, int k) { (void)p; (void)k; assert(locked); locked=0; }
static uint32_t k_cycle_get_32(void) { return ticks * 1000; }
static uint32_t k_cyc_to_us_floor32(uint32_t c) { return c; }
static int64_t k_uptime_get(void) { return ticks; }
static void maximum(int *p, uint32_t v) { if ((uint32_t)*p < v) *p=v; }
static void apex_radio_delivery_notify(void) {}
static void apex_radio_request_session(void) { requested++; }
#define shell_error(...) ((void)0)
#define shell_print(...) ((void)elapsed, (void)sh)
static void k_sleep(int ms) {
    assert(!locked); ticks += ms;
    if (scenario == 1) { input_selected=1; return; }
    if (scenario == 2) { sessions++; return; }
    if (scenario == 3) return;
    while (input_queue.count) {
        struct apex_input_frame *f = &input_queue.frames[input_queue.head];
        assert(f->type == APEX_INPUT_KEYBOARD && f->sequence == ++received);
        for (unsigned i=0; i<sizeof(f->data); i++) assert(f->data[i] == 0);
        input_queue.head = (input_queue.head+1) % APEX_INPUT_QUEUE_SIZE;
        input_queue.count--; input_acked++;
    }
}
'''
checks = r'''
int main(void) {
    struct shell sh;
    for (scenario=0; scenario<10; scenario++) {
        memset(&input_queue,0,sizeof(input_queue));
        input_queue.online=true; input_queue.next_sequence=1;
        input_selected=resync=input_acked=requested=ticks=received=queue_high_water=0;
        sessions=usb_ready=connected=1;
        if (scenario==4) input_selected=1;
        if (scenario==5) usb_ready=0;
        if (scenario==6) connected=0;
        if (scenario==7) input_queue.count=1;
        if (scenario==8) input_queue.next_sequence=UINT32_MAX;
        if (scenario==9) benchmark_running=1;
        int rc=apex_radio_benchmark(&sh,0,0);
        assert(clock_tx_lead_us==2000);
        assert(!locked && queue_high_water<=APEX_INPUT_QUEUE_SIZE);
        if (!scenario) assert(!rc && input_acked==1000 && received==1000 && !requested);
        if (scenario==1 || scenario==2) assert(rc==-ECANCELED && requested==1);
        if (scenario==3) assert(rc==-ETIMEDOUT && ticks==10000 && requested==1);
        if (scenario==4) assert(rc==-EBUSY && !input_queue.count && !requested);
        if (scenario>=5 && scenario<=7) assert(rc==-EBUSY && !requested);
        if (scenario==8) assert(rc==-EOVERFLOW && requested==1);
        if (scenario==9) assert(rc==-EBUSY && !requested);
    }
    benchmark_running=0;
    char *bad[] = {"", "499", "2001", "-1", "500x", "999999999999999999999"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        char *args[]={"radio_bench",bad[i]};
        assert(apex_radio_benchmark(&sh,2,args)==-EINVAL);
        assert(!benchmark_running && clock_tx_lead_us==2000);
    }
    scenario=0; ticks=received=input_acked=input_selected=resync=0;
    input_queue.head=input_queue.count=0; input_queue.online=true; input_queue.next_sequence=1;
    usb_ready=connected=1;
    char *args[]={"radio_bench","500"};
    assert(apex_radio_benchmark(&sh,2,args)==0);
    assert(received==1000 && clock_tx_lead_us==2000);
    ticks=received=input_acked=0;
    input_queue.head=input_queue.count=0; input_queue.next_sequence=1;
    char *paced[]={"radio_bench","500","paced"};
    assert(apex_radio_benchmark(&sh,3,paced)==0);
    assert(received==1000 && ticks>=1000 && ticks<=1001);
    paced[2]="invalid";
    assert(apex_radio_benchmark(&sh,3,paced)==-EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='apex-benchmark-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Benchmark tests passed')
