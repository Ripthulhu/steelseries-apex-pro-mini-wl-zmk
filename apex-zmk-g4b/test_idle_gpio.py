#!/usr/bin/env python3
"""Compile the real ATTN wait/ISR with mock GPIO and semaphore race injection."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (Path(__file__).resolve().parent / 'src/pins_g4b.c').read_text()
    functions = ''
    for signature in ('static void g4b_gpiote_attn_isr(', 'int g4b_attn_wait('):
        start = source.index(signature)
        functions += source[start:source.index('\n}\n', start) + 3]
    harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define BIT(x) (1u << (x))
#define K_MSEC(x) (x)
#define G4B_P0_ATTN 24
#define G4B_GPIOTE_ATTN_CH 1
#define G4B_GPIOTE_CONFIG_ATTN 0x00011801u
#define G4B_ATTN_SENSE_MASK (3u << 16)
#define G4B_ATTN_SENSE_HIGH (2u << 16)
#define GPIOTE_INTENCLR_PORT_Msk BIT(31)
#define GPIOTE_INTENSET_PORT_Msk BIT(31)
static struct { uint32_t CONFIG[8], EVENTS_IN[8], EVENTS_PORT,
                INTENCLR, INTENSET; } gpiote;
static struct { uint32_t PIN_CNF[32], LATCH, IN; } gpio;
#define NRF_GPIOTE (&gpiote)
#define NRF_P0 (&gpio)
static int g4b_attn_sem;
static uint32_t g4b_attn_isr_fires_ct;
static unsigned int locks, unlocks, barriers, takes, injection;
static void g4b_gpiote_attn_isr(const void *arg);
static void k_sem_give(int *sem) { assert(sem == &g4b_attn_sem); *sem = 1; }
static unsigned int irq_lock(void) { assert(!locks); locks = 1; return 7; }
static void edge(void) { gpio.IN |= BIT(G4B_P0_ATTN); gpiote.EVENTS_PORT = 1; }
static void barrier(void) {
    assert(locks);
    if (++barriers == 2 && injection == 1) edge(); /* before final level check */
}
#define __DSB() barrier()
static void irq_unlock(unsigned int key) {
    assert(locks && key == 7); locks = 0;
    if (++unlocks == 1) {
        if (injection == 2) edge(); /* after level check, before sem_take */
        if (gpiote.EVENTS_PORT || gpiote.EVENTS_IN[1]) g4b_gpiote_attn_isr(NULL);
    }
}
static int k_sem_take(int *sem, uint32_t timeout) {
    assert(sem == &g4b_attn_sem && timeout == 200 && !locks);
    assert(gpiote.CONFIG[0] == 0 && gpiote.CONFIG[1] == 0);
    assert((gpio.PIN_CNF[24] & G4B_ATTN_SENSE_MASK) == G4B_ATTN_SENSE_HIGH);
    assert(gpiote.INTENSET == GPIOTE_INTENSET_PORT_Msk);
    takes++;
    if (injection == 3) { edge(); g4b_gpiote_attn_isr(NULL); }
    if (*sem) { *sem = 0; return 0; }
    return -EAGAIN;
}
'''
    checks = r'''
static void reset_wait(uint32_t cfg) {
    assert(!locks);
    gpiote.CONFIG[0] = 0x00010501u;
    gpiote.CONFIG[1] = G4B_GPIOTE_CONFIG_ATTN;
    gpio.PIN_CNF[24] = cfg;
    gpio.IN = 0;
    gpiote.EVENTS_IN[1] = gpiote.EVENTS_PORT = 0;
    g4b_attn_sem = 0;
    unlocks = barriers = takes = 0;
}
static void restored(uint32_t cfg) {
    assert(!locks && unlocks == 2);
    assert(gpiote.CONFIG[0] == 0x00010501u);
    assert(gpiote.CONFIG[1] == G4B_GPIOTE_CONFIG_ATTN);
    assert(gpio.PIN_CNF[24] == cfg);
    assert(gpiote.INTENCLR == GPIOTE_INTENCLR_PORT_Msk);
    assert(gpiote.INTENSET == BIT(1) && gpiote.EVENTS_PORT == 0);
}
int main(void) {
    /* Exercise both event sources in the actual ISR. Both can share one wake. */
    for (int in = 0; in <= 1; in++) for (int port = 0; port <= 1; port++) {
        gpiote.EVENTS_IN[1] = in; gpiote.EVENTS_PORT = port;
        g4b_attn_sem = 0;
        uint32_t before = g4b_attn_isr_fires_ct;
        g4b_gpiote_attn_isr(NULL);
        assert(g4b_attn_sem == !!(in || port));
        assert(g4b_attn_isr_fires_ct == before + !!(in || port));
        assert(!gpiote.EVENTS_IN[1] && !gpiote.EVENTS_PORT);
    }
    /* Preserve input/pull/drive fields and an optional System OFF SENSE arm. */
    for (int sleep_arm = 0; sleep_arm <= 1; sleep_arm++) {
        uint32_t cfg = 0x30c | (sleep_arm ? G4B_ATTN_SENSE_HIGH : 0);
        for (injection = 0; injection <= 4; injection++) {
            reset_wait(cfg);
            if (injection == 4) gpio.IN = BIT(24); /* high before entry */
            int rc = g4b_attn_wait(200);
            assert(rc == (injection ? 0 : -EAGAIN));
            assert(takes == !(injection == 1 || injection == 4));
            restored(cfg);
        }
        /* A pre-check event can leave a semaphore token: the next wait consumes
         * it once, then later idle waits really block and restore both channels. */
        reset_wait(cfg); injection = 1;
        assert(g4b_attn_wait(200) == 0 && g4b_attn_sem == 1);
        injection = 0; gpio.IN = 0; unlocks = barriers = takes = 0;
        assert(g4b_attn_wait(200) == 0 && !g4b_attn_sem && takes == 1);
        restored(cfg);
        for (int repeat = 0; repeat < 10; repeat++) {
            unlocks = barriers = takes = 0;
            assert(g4b_attn_wait(200) == -EAGAIN && takes == 1);
            restored(cfg);
        }
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-idle-gpio-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + functions + checks)
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        str(path / 'test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('GPIO idle parking, restoration and attention wake race checks passed')


if __name__ == '__main__':
    main()
