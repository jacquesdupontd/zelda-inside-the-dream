#!/usr/bin/env python3
"""Analytic regression cases for the actual shared GLSL ray-march body.
This compiles that body's scalar arithmetic as C++; it is not GPU validation.
"""
import ctypes
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

class RayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[1]
        text = (root / "runtime/nvn/vulkan/xr_depth_ray.h").read_text()
        glsl = "".join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', text))
        body = glsl[glsl.index("float xr_surface("):]
        body = body.replace("pc.b.w", "flags").replace("textureSize(col,0).x", "width")
        body = body.replace("max(", "fmaxf(").replace("abs(", "fabsf(")
        src = """#include <cmath>
using std::fabs;
static int flags, width=1336, kind;
static float plane, origin, shift;
static float idist(float u, float) {
    if (kind==0) return plane;
    float t=(u-origin)/shift;
    return fabs(t-1.875f)<=0.025f ? 1.9f : 0.1f;
}
""" + body + """
extern "C" float trace(int f, float k, int scene, float depth, float u) {
    flags=f; kind=scene; plane=depth; origin=u; shift=k;
    return xr_surface(u,0.5f,k);
}
"""
        file = Path(cls.temp.name) / "ray.cpp"
        file.write_text(src)
        lib = file.with_suffix(".so")
        subprocess.run(["c++", "-std=c++17", "-O2", "-shared", "-fPIC", str(file), "-o", str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.trace = cls.lib.trace
        cls.trace.argtypes = [ctypes.c_int, ctypes.c_float, ctypes.c_int, ctypes.c_float, ctypes.c_float]
        cls.trace.restype = ctypes.c_float

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_planes_for_both_eyes_and_precision_modes(self):
        for flags in [0, 64, 128]:
            for k in [-0.08, -0.03, 0.03, 0.08]:
                for depth in [0.05, 0.1, 0.7, 1.9, 3.5, 5.0]:
                    with self.subTest(flags=flags, k=k, depth=depth):
                        self.assertAlmostEqual(self.trace(flags,k,0,depth,0.5), depth, places=4)

    def test_thin_surface_missed_by_legacy_grid(self):
        # Analytic foreground at inverse-distance1.9, background0.1.
        # Its projected footprint intersects the 24-step grid but not the 12-step grid.
        for k in [-0.03, 0.03]:
            for u in [0.2, 0.5, 0.8]:
                legacy = self.trace(0,k,1,0,u)
                matched = self.trace(64,k,1,0,u)
                self.assertAlmostEqual(legacy,0.1,places=4)
                self.assertLess(abs(matched-1.9),0.01)

    def test_colour_zero_shift_needs_no_depth_intersection(self):
        self.assertEqual(self.trace(64,0,0,1.9,0.5),0.0)

if __name__ == "__main__":
    unittest.main()
