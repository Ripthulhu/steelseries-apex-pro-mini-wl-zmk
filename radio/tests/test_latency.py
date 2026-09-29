#!/usr/bin/env python3
"""Check timing bucket boundaries and counter saturation with a host compiler."""
import argparse
from pathlib import Path
from host_test import compile_and_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = r'''
#include "apex_latency.h"
#include <assert.h>
#include <string.h>
int main(void) {
    struct apex_latency s = {0};
    uint32_t samples[] = {0, 1000, 1001, 2000, 2001, 5000,
                          5001, 10000, 10001, 20000, 20001, UINT32_MAX};
    uint64_t total = 0;
    for (unsigned int i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        apex_latency_add(&s, samples[i]);
        total += samples[i];
    }
    assert(s.count == 12 && s.min_us == 0 && s.max_us == UINT32_MAX);
    assert(s.total_us == total);
    for (unsigned int i = 0; i < 6; i++) assert(s.buckets[i] == 2);
    s.count = UINT32_MAX;
    struct apex_latency full = s;
    apex_latency_add(&s, 42);
    assert(!memcmp(&s, &full, sizeof(s)));
    return 0;
}
'''
    compile_and_run(source, cc=args.cc,
                    flags=('-I', str(Path(__file__).resolve().parents[1] / 'include')))
    print('Latency histogram tests passed')


if __name__ == '__main__':
    main()
