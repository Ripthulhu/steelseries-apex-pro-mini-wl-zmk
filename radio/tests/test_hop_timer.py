#!/usr/bin/env python3
"""Check timer arming races without accessing hardware."""
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
thread = source[source.index('static void probe_thread('):]
assert thread.index('int timeout_ms =') < thread.index('goto wait_next;')
start = source.index('static void hop_timer_arm(')
function = source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include <stdint.h>
#include <assert.h>
#define APEX_HOP_SLOT_US 20000u
#define APEX_INPUT_OPEN_US 750u
#define MIN(a,b) ((a)<(b)?(a):(b))
#define TIMER_INTENCLR_COMPARE0_Msk 1
#define TIMER_INTENSET_COMPARE0_Msk 1
#define TIMER2_IRQn 10
static struct { uint32_t INTENCLR, INTENSET, EVENTS_COMPARE[4], CC[4]; } timer;
static struct { uint32_t FREQUENCY; } radio;
#define NRF_TIMER2 (&timer)
#define NRF_RADIO (&radio)
static struct { struct { int running; } clock; } hop_link;
static int locked, position_error, channel, wakes, radio_event, hop_timer_late;
static uint32_t phase;
static uint64_t now, elapsed;
static int reads;
static unsigned int irq_lock(void) { assert(!locked); locked = 1; return 7; }
static void irq_unlock(unsigned int k) { assert(locked && k == 7); locked = 0; }
static void NVIC_ClearPendingIRQ(int irq) { assert(locked && irq == TIMER2_IRQn); }
static uint64_t radio_time_us(void) { assert(locked); return now + (reads++ ? elapsed : 0); }
#define apex_hop_link_channel(p, t) (channel)
static int apex_hop_position(void *p, uint64_t t, uint32_t *s, uint32_t *v) {
    (void)p; (void)t; *s = 6; *v = phase; return position_error;
}
static void k_sem_give(int *p) { assert(p == &radio_event); wakes++; }
#define atomic_inc(p) (++*(p))
'''
checks = r'''
int main(void) {
    for (int wrap = 0; wrap < 2; wrap++) {
        for (int scenario = 0; scenario < 6; scenario++) {
            timer.CC[0] = timer.INTENSET = 0;
            hop_link.clock.running = scenario != 0;
            position_error = scenario == 1;
            channel = scenario == 2 ? 50 : 26;
            radio.FREQUENCY = 26;
            phase = 19950;
            now = wrap ? UINT64_C(0xfffffff0) : 19950;
            elapsed = scenario == 4 ? 50 : scenario == 5 ? 51 : 2;
            reads = wakes = hop_timer_late = 0;
            hop_timer_arm(now + 10000);
            assert(!locked);
            if (scenario < 2) assert(!wakes && timer.CC[0] == (uint32_t)(now + 10000));
            else if (scenario == 2) assert(wakes == 1 && !timer.INTENSET);
            else {
                assert(timer.CC[0] == (uint32_t)(now + 50));
                assert(timer.INTENSET == 1);
                assert(wakes == (scenario >= 4));
                assert(hop_timer_late == (scenario >= 4));
            }
        }
    }
    for (phase = 0; phase < 20000; phase++) {
        hop_link.clock.running = 1; position_error = 0; channel = radio.FREQUENCY = 26;
        now = 100000 + phase; elapsed = 0; reads = 0;
        uint32_t delay = 20000 - phase;
        if (phase < 750) delay = 750 - phase;
        else if (phase < 3000) delay = 3000 - phase;
        delay = MIN(delay, 1500);
        hop_timer_arm(now + 1500);
        assert(timer.CC[0] == (uint32_t)(now + delay));
    }
}
'''
with tempfile.TemporaryDirectory(prefix='apex-hop-timer-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Hop timer tests passed')
