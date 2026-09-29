#!/usr/bin/env python3
"""Compile the runtime formatters and check their exact wire bytes, without hardware."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / 'src'

    def function(file, signature):
        text = (source / file).read_text()
        start = text.index(signature)
        return text[start:text.index('\n}', start) + 2]

    core = function('coredump_g4b.c', 'void g4b_coredump_emit_last(void)')
    radio = function('radio_g4b.c', 'static void standdown_emit(void)')
    ab = re.findall(r'n = snprintk\(.*?;\n\s*if \(n > 0\) \{.*?\n\s*}',
                    (source / 'ab_rollback_g4b.c').read_text(), re.DOTALL)
    assert len(ab) == 2, 'expected both rollback telemetry formatters'
    groups = (
        (('seq', 'seq'), ('rsn', 'reason'), ('rr', 'resetreas'), ('pc', 'pc'), ('lr', 'lr'), ('psr', 'xpsr')),
        tuple((name, name) for name in ('cfsr', 'hfsr', 'bfar', 'mmfar')),
        tuple((name, name) for name in ('r0', 'r1', 'r2', 'r3', 'r12')),
        tuple((f'r{i}', f'r{i}') for i in range(4, 8)),
        tuple((f'r{i}', f'r{i}') for i in range(8, 12)),
        (('sp', 'sp'), ('msp', 'msp'), ('psp', 'psp'), ('excret', 'exc_return'), ('sv', 'stack_valid')),
    )
    commands = ['g4b_coredump_emit_last();']  # no record: no output
    expected = []
    for seed in (0, 0x1234ab00, 0xffffffff):
        values = {field: (seed + i) & (0xffff if field == 'seq' else 0xffffffff)
                  for i, (_, field) in enumerate(pair for group in groups for pair in group)}
        commands += [f'g4b_cd_last.{field} = {value}u;' for field, value in values.items()]
        stack = [(seed + i) & 0xffffffff for i in range(32)]
        commands += [f'g4b_cd_last.stack[{i}] = {value}u;' for i, value in enumerate(stack)]
        commands += ['g4b_cd_have_last = true; g4b_coredump_emit_last();']
        expected += ['APXCD ' + ' '.join(f'{tag}={values[field]:08x}' for tag, field in group) + '\r\n'
                     for group in groups]
        expected += [f'APXCD stk{i:08x}=' + ''.join(f'{value:08x} ' for value in stack[i:i + 4]) + '\r\n'
                     for i in range(0, 32, 4)]
    for req, ready, down, error, wait in ((0, 0, 0, 0, 0), (1, 1, 1, 0, 50),
                                        (1, 1, 0, -123, 4000), (255, 2, 4, -32768, 65535)):
        commands += [f'status = (struct g4b_radio_status){{{req}, {ready}, {down}, {error}, {wait}}}; standdown_emit();']
        expected += [f'APXRADIO req={int(bool(req))} rdy={int(bool(ready))} down={int(bool(down))} err={error} wait={wait}\r\n']
    for armed in (0, 1):
        commands += [f'format_ab({armed});']
        expected += [f'APXAB fails=00000012 armed={armed} blen=00071000 bcrc=89abcdef thr={3 if armed else 0:08x}\r\n',
                     'APXISR fires=ffffffff\r\n']
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#define __packed __attribute__((packed))
#define snprintk snprintf
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#include "coredump_g4b.h"
#include "radio_g4b.h"
static struct g4b_coredump_record g4b_cd_last;
static bool g4b_cd_have_last;
static struct g4b_radio_status status;
static uint32_t g4b_attn_isr_fires(void) { return 0xffffffff; }
static void g4b_evidence_emit_text(const uint8_t *s, uint32_t n) {
    assert(n < 96 && n > 1 && s[n - 2] == '\r' && s[n - 1] == '\n');
    assert(s[n] == 0); /* the NUL terminator is never sent */
    assert(fwrite(s, 1, n, stdout) == n);
}
static void format_ab(bool armed) {
    char line[80]; int n; uint32_t fails = 0x12;
    struct { uint32_t b_len, b_crc32, fail_thresh; } h = {0x71000, 0x89abcdef, 3};
'''
    with tempfile.TemporaryDirectory(prefix='apex-evidence-format-') as folder:
        path = Path(folder)
        test = path / 'test.c'
        test.write_text(harness + '\n'.join(ab) + '\n}\n' + core + '\n' + radio +
                        '\nint main(void) {\n' + '\n'.join(commands) + '\nreturn 0;\n}\n')
        binary = path / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(source),
                        str(test), '-o', str(binary)], check=True)
        actual = subprocess.run([str(binary)], check=True, capture_output=True).stdout
        assert actual == ''.join(expected).encode(), 'telemetry wire format changed'
    print('Evidence formatting passed: no record, distinct/max register values, radio errors, rollback status')


if __name__ == '__main__':
    main()
