#!/usr/bin/env python3
"""Check keyboard RX gaps using the actual receive/ISR/wait code and mock MMIO."""
import argparse
from pathlib import Path
from host_test import compile_and_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / 'src/apex_radio_probe.c').read_text()
    functions = ''
    for name in ('radio_rx_needed', 'radio_receive', 'radio_isr'):
        # Match definitions, not the forward declaration of radio_receive.
        start = source.index(('static bool ' if name == 'radio_rx_needed' else 'static void ') +
                             name + '(')
        if source.index(';', start) < source.index('{', start):
            start = source.index('static void ' + name + '(', start + 1)
        functions += source[start:source.index('\n}\n', start) + 3]
    start = source.index('wait_next:\n') + len('wait_next:\n')
    idle = source[start:source.index('        maximum(&max_process_us', start)]
    harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define EVENT_RX 1
#define HOP_ENABLED 1
#define INPUT_ENABLED 1
#define APEX_KEYBOARD 0
#define ARG_UNUSED(x) (void)(x)
#define __DMB() ((void)0)
#define EGU_INTENSET_TRIGGERED0_Msk 1
#define EGU_INTENCLR_TRIGGERED0_Msk 1
#define RADIO_INTENSET_DISABLED_Msk 16
#define RADIO_INTENCLR_DISABLED_Msk 16
#define SWI3_EGU3_IRQn 1
#define RADIO_IRQn 2
#define NVIC_ClearPendingIRQ(x) ((void)(x))
static struct { uint32_t INTENSET, INTENCLR, EVENTS_TRIGGERED[1]; } egu;
static struct { uint32_t PACKETPTR, EVENTS_END, EVENTS_DISABLED, INTENSET,
                INTENCLR, TASKS_RXEN; } radio;
static struct { uint32_t CC[4]; } timer;
#define NRF_EGU3 (&egu)
#define NRF_RADIO (&radio)
#define NRF_TIMER2 (&timer)
static uint8_t dma[89];
static int local_role, hop_live, reply_pending, tx_pending, tx_done, radio_event;
static int rx_irq_pending, rx_irq_cycle, rx_interrupts, stop_calls, stop_error;
static int prepared;
static uint32_t tx_end_stamp;
static bool receiving;
#define atomic_get(p) (*(p))
#define atomic_set(p,v) (*(p) = (v))
#define atomic_inc(p) (++*(p))
static bool atomic_cas(int *p, int old, int value) {
    if (*p != old) return false;
    *p = value;
    return true;
}
static uint32_t k_cycle_get_32(void) { return 1234; }
static void k_sem_give(int *p) { ++*p; }
static int radio_stop(void) { stop_calls++; receiving = false; return stop_error; }
static void input_prepare_next(void) { prepared++; }
static void ack_prepare_next(void) {}
'''
    checks = r'''
int main(void) {
    /* Receiver, discovery/reconnect and every outstanding reply keep RX on.
     * Only an established keyboard with no reply pending may leave it off. */
    for (local_role = 0; local_role < 2; local_role++)
        for (hop_live = 0; hop_live < 2; hop_live++)
            for (reply_pending = 0; reply_pending < 2; reply_pending++) {
                bool needed = !(KEYBOARD_EVENT_RX && local_role == 0 &&
                                hop_live && !reply_pending);
                assert(radio_rx_needed() == needed);
                receiving = false; radio.TASKS_RXEN = radio.EVENTS_END = 0;
                radio_receive();
                assert(receiving == needed && radio.TASKS_RXEN == needed);
                stop_calls = prepared = 0;
                assert(wait_idle() == 0 && stop_calls == !needed);
                assert(prepared == 1); /* preparation continues while RX is parked */
            }
    local_role = 0; hop_live = 1; reply_pending = 0;
    /* Simulate a deferred TX: radio_send rearmed RX before its caller cleared
     * reply_pending. The end-of-loop cleanup must close it. */
    receiving = true; stop_calls = 0;
    assert(wait_idle() == 0);
    assert(receiving == !KEYBOARD_EVENT_RX && stop_calls == KEYBOARD_EVENT_RX);
    stop_error = -1;
    assert(wait_idle() == (KEYBOARD_EVENT_RX ? -1 : 0));
    stop_error = 0;
    /* A queued key, neutral gamepad, sync or shell TX sets reply_pending before
     * transmission. Its completion ISR must reopen RX before waking the owner. */
    reply_pending = tx_pending = 1; receiving = false; radio.TASKS_RXEN = 0;
    radio_isr(NULL);
    assert(receiving && radio.TASKS_RXEN && tx_done == 1 && !tx_pending);
    /* No reply (or an invalid one) cannot close the receive window. */
    assert(wait_idle() == 0 && receiving);
    /* Process a received frame before doing any speculative encryption. */
    radio.EVENTS_END = 1; prepared = 0;
    assert(wait_idle() == 0 && receiving && !prepared);
    /* Valid replies already clear reply_pending in input_packet/clock_ack_input.
     * No subsequent unsolicited packet is needed, including USB completion. */
    reply_pending = 0;
    assert(wait_idle() == 0 && receiving == !KEYBOARD_EVENT_RX);
    /* A session reset reopens discovery even with reply_pending cleared. */
    hop_live = 0; radio.TASKS_RXEN = 0;
    radio_receive();
    assert(receiving && radio.TASKS_RXEN);
    (void)egu; (void)radio_stop;
    return 0;
}
'''
    wait = 'static int wait_idle(void) {\n    int rc = 0;\n' + idle + \
           '    goto failed;\nfailed:\n    return rc;\n}\n'
    for keyboard in (0, 1):
        # Firmware pointers fit uint32_t; host pointers may not.
        compile_and_run(harness + functions + wait + checks, cc=args.cc,
                        flags=('-Wno-pointer-to-int-cast', f'-DKEYBOARD_EVENT_RX={keyboard}'))
    print('Keyboard RX idle-window tests passed; receiver behavior unchanged')


if __name__ == '__main__':
    main()
