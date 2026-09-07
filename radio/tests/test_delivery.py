#!/usr/bin/env python3
"""Compile and exercise the radio delivery queues without accessing hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    radio = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='apex-delivery-') as folder:
        binary = Path(folder) / 'delivery-test'
        subprocess.run([args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-I', str(radio / 'include'),
                        str(radio / 'tests/delivery_fixture.c'),
                        str(radio / 'src/apex_delivery.c'), str(radio / 'src/apex_input.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Delivery queue tests passed')


if __name__ == '__main__':
    main()
