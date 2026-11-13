#!/usr/bin/env python3
"""Exercise production dynamic-resolution decisions at 60/45/36 real images/s.

Synthetic timing inputs test the controller, not game speed or headset performance.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BudgetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "probe.c"
        cls.exe = Path(cls.tmp.name) / "probe"
        src.write_text('''#include "runtime/nvn/output_scale.c"
Settings g_settings;
const char *game_level_name(void) { return NULL; }
int main(int argc, char **argv) {
    double period = atof(argv[1]), gpu = atof(argv[2]);
    OutScale out;
    g_settings.res_mode = RES_DYNAMIC;
    for (int i = 1; i <= 600; i++)
        output_scale(1920, 1080, false, 1 + i * period / 1000, gpu, &out);
    printf("%.4f\\n", output_scale_dynamic_factor());
    return 0;
}
''')
        # Runtime headers use Clang's vector/elementwise builtins.
        subprocess.run(["clang", "-std=gnu11", "-O1", "-I", str(ROOT), str(src),
                        "-o", str(cls.exe), "-lm"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def factor(self, target, period, gpu):
        env = {k: v for k, v in os.environ.items() if not k.startswith("LARECOMP_")}
        env["LARECOMP_FRAME_AHEAD"] = "1"
        if target is not None:
            env["LARECOMP_DYNRES_TARGET_FPS"] = str(target)
        return float(subprocess.check_output([str(self.exe), str(period), str(gpu)],
                                             env=env, stderr=subprocess.DEVNULL, text=True))

    def test_45_budget_does_not_degrade_on_time_frames(self):
        self.assertEqual(self.factor(45, 1000 / 45, 18), 1)
        self.assertLess(self.factor(60, 1000 / 45, 18), 1)

    def test_36_budget_does_not_degrade_on_time_frames(self):
        self.assertEqual(self.factor(36, 1000 / 36, 23), 1)

    def test_over_budget_still_reduces_resolution(self):
        self.assertLess(self.factor(45, 30, 28), 1)
        self.assertLess(self.factor(36, 40, 35), 1)

    def test_default_and_invalid_targets_keep_60_behavior(self):
        expected = self.factor(60, 22, 18)
        for target in (None, "nan", "inf", "", "0", "-45", "45junk", "500"):
            with self.subTest(target=target):
                self.assertEqual(self.factor(target, 22, 18), expected)


if __name__ == "__main__":
    unittest.main()
