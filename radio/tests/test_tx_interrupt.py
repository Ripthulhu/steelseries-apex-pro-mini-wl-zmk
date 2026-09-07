#!/usr/bin/env python3
"""Exercise the TX-completion ISR for both the direct RADIO and EGU routes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
start = source.index('static void radio_isr(')
handler = source[start:source.index('\n}\n', start) + 3]
assert 'while (' not in handler
assert 'NRF_PPI->CH[19].EEP = (uint32_t)&NRF_RADIO->EVENTS_DISABLED;' in source
send = source[source.index('static int radio_send('):source.index('static int radio_init(')]
assert send.index('unsigned int tx_key = irq_lock()') < send.index('k_sem_reset(&tx_done)')
assert send.index('NVIC_ClearPendingIRQ(SWI3_EGU3_IRQn)') < send.index('atomic_set(&tx_pending, 1)')
assert send.index('NVIC_ClearPendingIRQ(RADIO_IRQn)') < send.index('atomic_set(&tx_pending, 1)')
assert send.index('atomic_set(&tx_pending, 1)') < send.index('radio_start_immediate(packet[1])')
assert send.count('irq_unlock(tx_key)') == 3
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define ARG_UNUSED(x) (void)(x)
#define HOP_ENABLED 1
#define RADIO_STATE_STATE_Disabled 0
#define EGU_INTENCLR_TRIGGERED0_Msk 1
#define RADIO_INTENCLR_DISABLED_Msk 1
static struct { uint32_t STATE, INTENCLR; } radio;
static struct { uint32_t CC[4]; } timer;
static struct { uint32_t INTENCLR, EVENTS_TRIGGERED[1]; } egu;
#define NRF_RADIO (&radio)
#define NRF_TIMER2 (&timer)
#define NRF_EGU3 (&egu)
static int tx_pending, tx_done, radio_event, rx_irq_cycle, rx_irq_pending, rx_interrupts;
static uint32_t tx_end_stamp, ticks, receive_calls;
static bool atomic_cas(int *p, int old, int value) {
    if (*p != old) return false;
    *p = value; return true;
}
#define atomic_set(p,v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
static uint32_t k_cycle_get_32(void) { return ticks++; }
static void radio_receive(void) { assert(radio.STATE == 0); receive_calls++; }
static void k_sem_give(int *sem) {
    if (sem == &tx_done && tx_pending == 0) assert(receive_calls);
    ++*sem;
}
'''
checks = r'''
int main(void) {
    (void)egu;
    tx_pending = 1; timer.CC[2] = 1234;
    radio_isr(0);
    assert(tx_pending == 0 && tx_done == 1 && receive_calls == 1);
    assert(tx_end_stamp == 1234 && rx_interrupts == 0);
    radio_isr(0);
    assert(rx_interrupts == 1 && radio_event == 1 && rx_irq_pending == 1);
    assert(tx_done == 1 && receive_calls == 1);
    /* A delayed interrupt still completes TX without a software timing limit. */
    tx_pending = 1; ticks = 392000;
    radio_isr(0);
    assert(tx_pending == 0 && tx_done == 2 && receive_calls == 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apex-tx-irq-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + handler + checks)
    for keyboard in (0, 1):
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        f'-DKEYBOARD_EVENT_RX={keyboard}', str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)
print('Transmit interrupt tests passed')
