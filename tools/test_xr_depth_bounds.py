#!/usr/bin/env python3
"""Check that conservative depth bounds cannot skip the original first hit.

This exercises discontinuities, near surfaces, sky, cropped coordinates, clamped
edges, both eye offsets, odd image widths and the upward RGBA8 quantization.
The GPU's actual colour output is checked separately with XR_WARP_COMPARE_AT.
"""
import math
import random
import unittest


def round_up(value):
    return min(1.0, (math.ceil(value * 255.0) + 1.0) / 255.0)


class DepthBoundsTest(unittest.TestCase):
    def test_skipped_samples_cannot_hit(self):
        rng = random.Random(20261025)
        skipped = 0
        for width in (1, 63, 64, 65, 1919, 3344):
            depth = [rng.choice((0.0, 0.01, 0.3, 0.9, 2.2)) for _ in range(width)]
            if width > 64:
                depth[width // 2] = 8.0  # close occluder in one tile only
            # Two reductions by eight, each rounding the maximum upwards.
            by8 = [round_up(max(min(x, 5.0) / 5.0 for x in depth[i:i + 8]))
                   for i in range(0, width, 8)]
            bounds = [round_up(max(by8[i:i + 8])) * 5.0 for i in range(0, len(by8), 8)]

            def pixel(u):
                return min(width - 1, max(0, math.floor(u * width)))

            for _ in range(2000):
                steps = rng.choice((12, 24, 48))
                u = rng.uniform(-0.1, 1.1)
                shift = rng.uniform(-0.025, 0.025)
                lo, hi = sorted((pixel(u), pixel(u + shift * 5.0)))
                bound = max(bounds[lo // 64:hi // 64 + 1])
                start = min(steps, max(1, math.floor(steps * (1 - bound / 5.0))))
                # Every skipped reference sample must be in empty space. The
                # sample preceding start is re-evaluated by the GPU, so the
                # refinement's bracket and its sampled values remain unchanged.
                for step in range(start):
                    t = 5.0 * (1 - step / float(steps))
                    d = depth[pixel(u + shift * t)] - t
                    if start > 1:
                        self.assertLess(d, 0.0)
                        skipped += 1
                for x in range(lo, hi + 1):
                    self.assertGreaterEqual(bound, min(depth[x], 5.0))
        self.assertGreater(skipped, 10000)

    def test_quantization_is_conservative(self):
        for n in range(65537):
            value = n / 65536
            self.assertGreaterEqual(round_up(round_up(value)), value)


if __name__ == "__main__":
    unittest.main()
