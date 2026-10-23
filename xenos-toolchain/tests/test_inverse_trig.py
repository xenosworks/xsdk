#!/usr/bin/env python3
"""asin, acos and atan of a vector, each lane its own (xsdk:src/xsc_lower_glsl.c, lanes, asin_reg and atan_reg).

vk_inverse_trig.frag takes asin, acos and atan of a vec4 whose lanes differ, and asin and acos of a float. Each runs on xdis/simulate.py,
the optimiser off and on, and every lane must be within the lowering's polynomial of the true value: Abramowitz and
Stegun's 4.4.45 for asin, |error| at most 6.7e-5 rad, and an odd polynomial for atan on -1 to 1, given 1e-4 here for
single rounding. Until milestone 61 the lowering took a vector's first lane for all four (broadcast) and asin one
square root, so every lane gave lane x's answer; the inputs' lanes differ enough to show it.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

NAME = "vk_inverse_trig.frag"
X = [simulate.single(v) for v in (0.95, -0.6, 0.1, -0.25)]
Y = simulate.single(0.7)
TOLERANCE = 1e-4


def main():
    want = {
        0: [math.asin(v) for v in X],
        1: [math.acos(v) for v in X],
        2: [math.asin(Y), math.acos(Y)],
        3: [math.atan(v) for v in X],
    }
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
        got = simulate.run(blob, {0: X, 1: [Y, Y, Y, Y]}, literals).exports
        for export, values in want.items():
            lanes = got[export][: len(values)]
            off = [abs(g - w) for g, w in zip(lanes, values)]
            if max(off) > TOLERANCE:
                raise SystemExit(f"  FAIL {NAME}, {tag}: export {export} {lanes}, not {values} within {TOLERANCE}")
        print(f"  PASS {NAME}, {tag}: every lane within {TOLERANCE} rad")
    print("OK")


if __name__ == "__main__":
    main()
