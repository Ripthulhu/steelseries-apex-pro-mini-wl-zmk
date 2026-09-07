#!/usr/bin/env python3
"""Check the timed TX deadline guard, including hardware timer wrap."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
start = source.index('static int radio_arm_clock(')
function = source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#define BIT(n) (1u << (n))
static struct { uint32_t CHENCLR, CHENSET; } ppi;
static struct { uint32_t CC[4], EVENTS_COMPARE[4]; } timer;
#define NRF_PPI (&ppi)
#define NRF_TIMER2 (&timer)
static int locked;
static uint64_t now;
static unsigned int irq_lock(void) { assert(!locked); locked=1; return 9; }
static void irq_unlock(unsigned int key) { assert(locked && key==9); locked=0; }
static uint64_t radio_time_us(void) { assert(locked); return now; }
'''
checks = r'''
int main(void) {
    for (int wrap=0;wrap<2;wrap++) {
        now=wrap ? UINT64_C(0xfffffff0) : 2000;
        int deltas[]={-1,0,99,100,101,500,2000};
        for (unsigned i=0;i<sizeof(deltas)/sizeof(deltas[0]);i++) {
            uint64_t deadline=now+deltas[i];
            ppi.CHENSET=ppi.CHENCLR=0; timer.EVENTS_COMPARE[1]=1;
            assert(radio_arm_clock(deadline)==(deltas[i]<=100 ? -EAGAIN : 0));
            assert(!locked && ppi.CHENCLR==BIT(18));
            assert(ppi.CHENSET==(deltas[i]<=100 ? 0 : BIT(18)));
            assert(timer.CC[1]==(uint32_t)deadline && timer.EVENTS_COMPARE[1]==0);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='apex-clock-arm-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Clock arming tests passed')
