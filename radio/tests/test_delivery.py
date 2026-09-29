#!/usr/bin/env python3
"""Compile and exercise the radio delivery queues without accessing hardware."""
import argparse
from pathlib import Path
from host_test import compile_and_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    radio = Path(__file__).resolve().parents[1]
    compile_and_run((radio / 'tests/delivery_fixture.c').read_text(), cc=args.cc,
                    flags=('-fsanitize=undefined', '-I', str(radio / 'include')),
                    sources=(radio / 'src/apex_delivery.c', radio / 'src/apex_input.c'))
    print('Delivery queue tests passed')


if __name__ == '__main__':
    main()
