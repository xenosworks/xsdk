#!/usr/bin/env python3
"""pow with a vector exponent takes each lane's own (GLSL.std.450 Pow), where xost once multiplied every lane's log by
the exponent's x (xsdk:src/xsc_lower_glsl.c, power).

vk_pow_lanes.frag raises (2, 3, 5) to (1, 2, 0.5) on xdis/simulate.py, the optimiser off and on: the exports must be
2, 9 and 5's square root within the scalar unit's rounding, and not (2, 3, 5), what x's exponent alone gives.
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

NAME = "vk_pow_lanes.frag"
BASE = [2.0, 3.0, 5.0, 1.0]
POWER = [1.0, 2.0, 0.5, 1.0]
# exp2 of log2 in the scalar unit is within a few ulps.
TOLERANCE = 1e-5


def main():
    want = [b ** p for b, p in zip(BASE[:3], POWER[:3])] + [1.0]
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
        got = simulate.run(blob, {0: BASE, 1: POWER}, literals).exports[0]
        if any(not math.isclose(g, w, rel_tol=TOLERANCE) for g, w in zip(got, want)):
            raise SystemExit(f"  FAIL {NAME}, {tag}: exported {got} where pow gives {want}")
        print(f"  ok {NAME}, {tag}: {got}")
    print("pow lanes: ok")


if __name__ == "__main__":
    main()
