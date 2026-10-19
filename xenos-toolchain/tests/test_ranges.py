#!/usr/bin/env python3
"""Clamps and a wrap a value's range makes useless, dropped (xsdk:src/xsc_optimize.c, fold_by_ranges).

vk_ranges.frag clamps a product of saturated values to ±4 and then to 0 to 1, takes a min and a max of a saturated
value with its own ends, and wraps a saturated product times 255 at 256, whose floor is 0: each goes. It keeps what
the ranges don't settle: a clamp of an input; a min of a value that may be NaN, which gives 4 where it is; a max of -a
and a, -0 where a is +0, and its clamp, which gives +0 there, so 1 / x gives +inf; and, each biting at its operand's
top, a saturated value's floor, a min of a product with 1, the floor of a product whose exact top is below 1 but
rounds to 1. Each runs on xdis/simulate.py, the optimiser off and on, on inputs at those edges, a NaN among them, and
every export must be the same bit for bit, the edges' lane 275, the sum the kept floors and mins make there. Then the
ALU instructions are what the drops leave, and tools/needless.py finds no range-useless op left but the max of -a and
a, as it doesn't tell -0 from +0.
"""
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import needless  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
from test_local_stores import module  # noqa: E402
from test_optimize import alu_count  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

NAME = "vk_ranges.frag"
# Lane x is the edges' (a 1, p 1), y has a b that is NaN, z an a of +0.
A = [1.5, 0.3, -0.7, 0.999]
B = [3.0, math.nan, 1.1, -2.0]
EDGES = 275.0
RANGE_USELESS = {False: 14, True: 1}
ALU = {False: 48, True: 23}


def bits(exports):
    return {reg: [struct.pack(">f", v) for v in values] for reg, values in exports.items()}


def main():
    s = simulate.single
    if s(s(1.0 * 0.14285715) * 6.9999995) != 1.0 or 0.14285715 * 6.9999995 >= 1.0:
        raise SystemExit("  FAIL the constants: their exact product must be below 1 and round to 1")
    runs = {}
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
        got = runs[optimize] = simulate.run(blob, {0: A, 1: B}, literals).exports
        if any(math.isnan(v) for values in got.values() for v in values) or got[3][0] != EDGES:
            raise SystemExit(f"  FAIL {NAME}, {tag}: exports {got}, a NaN or the edges' lane not {EDGES}")
        found = {"range-useless": needless.count(blob, manifest)["range_useless"], "ALU": alu_count(blob)}
        want = {"range-useless": RANGE_USELESS[optimize], "ALU": ALU[optimize]}
        if found != want:
            raise SystemExit(f"  FAIL {NAME}, {tag}: {found}, not {want}")
        print(f"  PASS {NAME}, {tag}: {found}")
    if bits(runs[True]) != bits(runs[False]):
        raise SystemExit(f"  FAIL {NAME}: exports {runs[True]} with the optimiser on, {runs[False]} off")
    print(f"  PASS {NAME}: exports the same bit for bit")
    print("OK")


if __name__ == "__main__":
    main()
