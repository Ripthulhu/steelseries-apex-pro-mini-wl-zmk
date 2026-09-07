#!/usr/bin/env python3
"""Test both receive interrupt handlers without touching hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / 'src/apex_radio_probe.c').read_text()
    start = source.index('static void radio_isr(')
    end = source.index('\n}\n', start) + 3
    handler = source[start:end]
    harness = r'''
#include <stdint.h>
#include <assert.h>
#define ARG_UNUSED(x) (void)(x)
#define EGU_INTENCLR_TRIGGERED0_Msk 1u
#define RADIO_INTENCLR_DISABLED_Msk 16u
#define atomic_set(p,v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
static struct { uint32_t INTENCLR, EVENTS_TRIGGERED[16]; } egu;
static struct { uint32_t INTENCLR, STATE; } radio;
#define RADIO_STATE_STATE_Disabled 0
static int tx_pending, tx_done;
static int atomic_cas(int *p, int old, int value) {
    if (*p != old) return 0;
    *p = value; return 1;
}
static void radio_receive(void) { assert(0); }
#define NRF_EGU3 (&egu)
#define NRF_RADIO (&radio)
static uint32_t rx_irq_cycle, rx_irq_pending, rx_interrupts;
static int radio_event;
static uint32_t k_cycle_get_32(void) { return 1234; }
static void k_sem_give(int *p) { (*p)++; }
'''
    checks = r'''
int main(void) {
    egu.INTENCLR = 99; egu.EVENTS_TRIGGERED[0] = 1;
    radio.INTENCLR = 77;
    radio_isr(0);
    assert(rx_irq_cycle == 1234 && rx_irq_pending == 1);
    assert(rx_interrupts == 1 && radio_event == 1);
#if KEYBOARD_EVENT_RX
    assert(egu.INTENCLR == 1 && egu.EVENTS_TRIGGERED[0] == 0);
    assert(radio.INTENCLR == 77);
#else
    assert(radio.INTENCLR == 16 && egu.INTENCLR == 99);
    assert(egu.EVENTS_TRIGGERED[0] == 1);
#endif
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-rx-test-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(harness + handler + checks)
        binary = path / 'test.exe'
        for keyboard in (0, 1):
            subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                            f'-DKEYBOARD_EVENT_RX={keyboard}', str(path / 'test.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print('Receive interrupt tests passed for keyboard and receiver')


if __name__ == '__main__':
    main()
