#!/usr/bin/env python3
"""Test the actual input preparation helpers with a counted encoder."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apex_radio_probe.c').read_text()
start = source.index('static struct {\n    uint8_t packet[APEX_PACKET_MAX]')
declarations = source[start:source.index('\nstatic atomic_t pause_requested', start)]
functions = ''
for name in ('input_prepare_clear', 'input_prepared_current', 'input_prepare_take', 'input_prepare_next'):
    start = source.index(name + '(')
    start = source.rfind('static ', 0, start)
    functions += source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include "apex_input.h"
#include "apex_delivery.h"
#include <assert.h>
#include <string.h>
#define APEX_PACKET_MAX 88
#define APEX_KEYBOARD 0
#define APEX_CONNECTION_ESTABLISHED 5
#define APEX_PACKET_INPUT 1
typedef int atomic_t;
typedef int k_spinlock_key_t;
#define atomic_get(p) (*(p))
#define atomic_inc(p) (++*(p))
static int local_role, resync, input_lock, encode_calls, encode_error;
static struct { struct { uint32_t tx_counter; uint8_t id[8]; } session; int state; } connection;
static struct apex_input_queue input_queue;
static struct apex_delivery_tx delivery_tx;
static int k_spin_lock(int *p) { (void)p; return 0; }
static void k_spin_unlock(int *p, int key) { (void)p; (void)key; }
static int apex_connection_encode(void *c, int type, const uint8_t *payload,
                                  int length, uint8_t *out, unsigned int capacity) {
    (void)c; assert(type == APEX_PACKET_INPUT && capacity >= (unsigned int)length);
    encode_calls++;
    if (encode_error) return -1;
    connection.session.tx_counter++;
    memcpy(out, payload, length);
    return length;
}
'''
checks = r'''
int main(void) {
    uint8_t out[88];
    connection.state = 5; connection.session.tx_counter = 10;
    apex_input_session(&input_queue);
    delivery_tx.sent = 1;
    input_prepare_next();
    assert(encode_calls == 1 && input_prepared == 1 && prepared_input.sequence == 2);
    input_prepare_next();
    assert(encode_calls == 1);
    assert(input_prepare_take(2, out) == 18 && input_prepare_used == 1);
    assert(out[0] == APEX_INPUT_VERSION && out[1] == 2);
    assert(!input_prepare_take(2, out));
    input_prepare_next();
    unsigned int reserved = connection.session.tx_counter;
    connection.session.tx_counter++; /* Clock packet encoded after preparation. */
    assert(!input_prepare_take(2, out));
    assert(connection.session.tx_counter == reserved + 1);
    input_prepare_next();
    connection.session.id[0]++; /* New session, even with a coincident counter. */
    assert(!input_prepare_take(2, out));
    input_prepare_next();
    assert(!input_prepare_take(1, out)); /* Retransmission needs a fresh encode. */
    assert(input_prepare_discarded == 3);
    input_prepare_next(); input_prepare_clear();
    assert(!prepared_input.length && prepared_input.sequence == 0);
    int before = encode_calls;
    resync = 1; input_prepare_next(); assert(encode_calls == before);
    resync = 0; local_role = 1; input_prepare_next(); assert(encode_calls == before);
    local_role = 0; connection.state = 0; input_prepare_next(); assert(encode_calls == before);
    connection.state = 5; input_queue.online = false;
    input_prepare_next(); assert(encode_calls == before);
    input_queue.online = true; delivery_tx.sent = UINT32_MAX;
    input_prepare_next(); assert(encode_calls == before);
    delivery_tx.sent = 1; encode_error = 1;
    input_prepare_next(); assert(!prepared_input.length);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apex-input-prepare-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(harness + declarations + functions + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    str(root / 'src/apex_input.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Input preparation tests passed')
