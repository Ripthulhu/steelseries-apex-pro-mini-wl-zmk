#!/usr/bin/env python3
"""Exercise prepared replies against changing queue, LED and session state."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
start = source.index('static struct {\n    uint8_t packet[APEX_PACKET_MAX], payload')
code = source[start:source.index('static void input_prepare_clear(', start)]
harness = r'''
#include "apex_delivery.h"
#include "apex_latency.h"
#include <assert.h>
#include <string.h>
#define APEX_PACKET_MAX 88
#define APEX_RECEIVER 1
#define APEX_CONNECTION_ESTABLISHED 5
#define APEX_PACKET_ACK 2
#define ARG_UNUSED(x) (void)(x)
typedef int atomic_t;
typedef int k_spinlock_key_t;
#define atomic_get(p) (*(p))
#define atomic_inc(p) (++*(p))
static int local_role = 1, resync, input_lock, encode_calls, fail_encode;
static struct { struct { uint32_t tx_counter; uint8_t id[8]; } session; int state; } connection;
static struct apex_delivery_ack status;
static struct apex_latency ack_encode_latency;
static uint8_t tx[APEX_PACKET_MAX];
static void apex_radio_delivery_status(struct apex_delivery_ack *ack) { *ack = status; }
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int k) { (void)p; (void)k; }
static uint32_t k_cycle_get_32(void) { return 100; }
#define k_cyc_to_us_floor32(x) (x)
static int apex_connection_encode(void *c, int type, const uint8_t *in, unsigned int n,
                                  uint8_t *out, unsigned int size) {
    (void)c; assert(type == APEX_PACKET_ACK && size >= n); encode_calls++;
    if (fail_encode) return -1;
    connection.session.tx_counter++;
    memcpy(out, in, n); return n;
}
'''
checks = r'''
int main(void) {
    connection.state = 5; status.free = 8;
    ack_prepare_next(); ack_prepare_next();
    assert(encode_calls == 1 && ack_prepared == 1);
    /* Use only after the predicted report was authenticated and accepted. */
    status.accepted++; status.free--;
    assert(input_ack(1) == 11 && ack_prepare_used == 1 && encode_calls == 1);
    assert(!prepared_ack.length);
    struct apex_delivery_ack decoded;
    assert(apex_delivery_ack_unpack(tx, 11, &decoded));
    assert(decoded.accepted == 1 && decoded.completed == 0);
    assert(input_ack(1) == 11 && encode_calls == 2); /* Never reuse a nonce. */
    for (int change = 0; change < 5; change++) {
        status = (struct apex_delivery_ack){10, 9, 7, 0};
        ack_prepare_next();
        uint32_t counter = connection.session.tx_counter;
        status.accepted++; status.free--;
        if (change == 0) { status.completed++; status.free++; }
        if (change == 1) status.leds = 1;
        if (change == 2) connection.session.tx_counter++;
        if (change == 3) connection.session.id[0]++;
        if (change == 4) { status.accepted--; status.free++; } /* Retry/full FIFO. */
        int before = encode_calls;
        assert(input_ack(status.accepted) == 11 && encode_calls == before + 1);
        assert(connection.session.tx_counter > counter);
        assert(!prepared_ack.length && ack_prepare_used == 1);
    }
    ack_prepare_next();
    uint32_t counter = connection.session.tx_counter;
    ack_prepare_clear(); assert(connection.session.tx_counter == counter);
    int before = encode_calls;
    status.free = 0; ack_prepare_next(); assert(encode_calls == before);
    status.free = 8; status.accepted = UINT32_MAX;
    ack_prepare_next(); assert(encode_calls == before);
    status.accepted = 0; resync = 1; ack_prepare_next(); assert(encode_calls == before);
    resync = 0; local_role = 0; ack_prepare_next(); assert(encode_calls == before);
    local_role = 1; connection.state = 0; ack_prepare_next(); assert(encode_calls == before);
    connection.state = 5; fail_encode = 1; ack_prepare_next(); assert(!prepared_ack.length);
}
'''
with tempfile.TemporaryDirectory(prefix='apex-ack-prepare-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + code + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(root / 'include'),
                    str(path / 'test.c'), str(root / 'src/apex_delivery.c'),
                    str(root / 'src/apex_input.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Prepared ACK tests passed')
