#!/usr/bin/env python3
"""Check that compact scanner storage preserves every request and reply."""
import unittest

from build_scanner_config import parse_capture
from compact_boot_prefix import compact, render


class CompactPrefixTests(unittest.TestCase):
    def test_round_trip(self):
        frames = parse_capture()
        blocks, exchanges = compact(frames)
        for (_, tx, rx), (ti, ri) in zip(frames, exchanges):
            self.assertEqual(blocks[ti], bytes(tx))
            self.assertEqual(blocks[ri], bytes(rx))
        # Cortex-M uses two four-byte flash pointers per exchange.
        self.assertLess(len(blocks) * 64 + len(exchanges) * 8, 59 * 128)

    def test_deterministic(self):
        self.assertEqual(render(parse_capture()), render(parse_capture()))

    def test_missing_exchange(self):
        with self.assertRaises(ValueError):
            compact(parse_capture()[:-1])

    def test_wrong_order(self):
        with self.assertRaises(ValueError):
            compact(list(reversed(parse_capture())))

    def test_short_frame(self):
        frames = parse_capture()
        frames[0] = (1, frames[0][1][:-1], frames[0][2])
        with self.assertRaises(ValueError):
            compact(frames)


if __name__ == "__main__":
    unittest.main()
