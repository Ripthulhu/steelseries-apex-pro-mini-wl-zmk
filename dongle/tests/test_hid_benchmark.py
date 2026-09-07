#!/usr/bin/env python3
"""Check bounded USB-only benchmarking and radio recovery on every exit."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'dongle/src/hid.c').read_text()
start = source.index('int receiver_hid_benchmark(')
function = source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include "apex_input.h"
#include <assert.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define K_MSEC(x) (x)
struct shell { int unused; };
typedef int k_spinlock_key_t;
static int lock, ready, busy, release_mask, resumes, releases, scenario;
static uint32_t ticks, sent_count, done_count;
static bool apex_radio_pause(void) { return scenario != 1; }
static void apex_radio_resume(void) { resumes++; }
static void apex_radio_release(void) { releases++; release_mask=3; }
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int key) { (void)p; (void)key; }
static int64_t k_uptime_get(void) { return ticks; }
static uint32_t k_cycle_get_32(void) { return ticks*1000; }
#define k_cyc_to_us_floor32(x) (x)
static void k_sleep(int ms) {
    ticks+=ms; release_mask=0;
    if (sent_count>done_count && scenario!=3) done_count++;
}
static int apex_radio_deliver(const struct apex_input_frame *f) {
    if (scenario==4) return -EIO;
    if (sent_count-done_count>=8) return -EAGAIN;
    assert(f->sequence==sent_count+1 && f->type==1);
    for (unsigned int i=0;i<sizeof(f->data);i++) assert(f->data[i]==0);
    sent_count++; return 0;
}
static uint32_t apex_radio_completed(uint32_t *at) { *at=ticks*1000; return done_count; }
#define shell_print(...) ((void)sh,(void)elapsed)
'''
checks = r'''
int main(void) {
    struct shell sh;
    for (scenario=0;scenario<5;scenario++) {
        ready=scenario!=2; busy=resumes=releases=ticks=sent_count=done_count=0;
        int rc=receiver_hid_benchmark(&sh,0,0);
        if (scenario==1) { assert(rc==-EBUSY && resumes==0 && releases==0); continue; }
        assert(resumes==1 && releases==2 && release_mask==3);
        if (scenario==0) assert(!rc && sent_count==1000 && done_count==1000 && ticks==1001);
        if (scenario==2) assert(rc==-ETIMEDOUT && ticks==1000 && sent_count==0);
        if (scenario==3) assert(rc==-ETIMEDOUT && ticks==5001 && done_count==0);
        if (scenario==4) assert(rc==-EIO && sent_count==0);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='apex-usb-benchmark-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-I', str(root / 'radio/include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('USB-only benchmark tests passed')
