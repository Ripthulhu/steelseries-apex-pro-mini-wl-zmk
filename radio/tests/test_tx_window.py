#!/usr/bin/env python3
"""Test the transport's final transmit check without accessing hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/apex_radio_probe.c').read_text()
    start = source.index('static int radio_start_immediate(')
    function = source[start:source.index('\n}\n', start) + 3]
    header = (root / 'include/apex_packet.h').read_text()
    enum_start = header.index('enum apex_packet_type {')
    packet_types = header[enum_start:header.index('};', enum_start) + 2]
    harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define APEX_RECEIVER 1
static struct { unsigned int TASKS_TXEN, FREQUENCY; } radio;
#define NRF_RADIO (&radio)
static int locked, tx_window_deferrals;
static unsigned int irq_lock(void) { assert(!locked); locked = 1; return 7; }
static void irq_unlock(unsigned int key) { assert(locked && key == 7); locked = 0; }
#define atomic_inc(p) (++*(p))
#if HOP_ENABLED
static int window, reply_window, channel, hop_link;
static int local_role; /* APEX_KEYBOARD; only read on the SHELL reply path */
static uint64_t radio_time_us(void) { assert(locked); return 1234; }
static int apex_hop_link_input_window(int *p, uint64_t now) {
    assert(locked && p == &hop_link && now == 1234); return window;
}
static int apex_hop_link_reply_window(int *p, uint64_t now) {
    assert(locked && p == &hop_link && now == 1234); return reply_window;
}
static int apex_hop_link_channel(int *p, uint64_t now) {
    assert(locked && p == &hop_link && now == 1234); return channel;
}
#endif
'''
    checks = r'''
int main(void) {
    uint8_t types[] = {APEX_PACKET_INPUT, APEX_PACKET_ACK, APEX_PACKET_KEEPALIVE, APEX_PACKET_INPUT_BATCH,
                       APEX_PACKET_CONTROL, APEX_PACKET_CHANNEL_MAP, 0x80};
    for (unsigned int i = 0; i < sizeof(types); i++) {
        for (int scenario = 0; scenario < 4; scenario++) {
            radio.TASKS_TXEN = tx_window_deferrals = 0;
            radio.FREQUENCY = 26;
            int defer = 0;
#if HOP_ENABLED
            window = scenario != 0 && scenario != 3;
            reply_window = scenario != 0;
            channel = scenario == 1 ? 50 : 26;
            defer = i < 4 && scenario < 2;
            if (scenario == 3) defer = i < 4 && types[i] != APEX_PACKET_ACK;
#endif
            assert(radio_start_immediate(types[i]) == (defer ? -EAGAIN : 0));
            assert(!locked && radio.TASKS_TXEN == (unsigned int)!defer);
            assert(tx_window_deferrals == defer);
        }
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='apex-tx-window-') as folder:
        path = Path(folder)
        (path / 'test.c').write_text(packet_types + harness + function + checks)
        binary = path / 'test.exe'
        for hopping in (0, 1):
            subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                            f'-DHOP_ENABLED={hopping}', '-I', str(root / 'include'),
                            str(path / 'test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print('Transmit window tests passed for hopping and fixed-channel builds')


if __name__ == '__main__':
    main()
