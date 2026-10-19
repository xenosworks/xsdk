#!/usr/bin/env python3
"""mix with a constant 0 or 1, and adds, muls and mads with a known 0 or 1, folded to what remains (xsdk:
src/xsc_lower_glsl.c, mix; src/xsc_optimize.c, fold_by_ranges).

vk_identities.frag mixes with a weight of 1, a weight of 0, an x of 0 and a y of 0, takes x * 1, 0 - y and a * 0 of
vectors, and mixes with a 0 and multiplies by a 1 that only the optimiser sees, through the vector they're built into.
Each lane runs on xdis/simulate.py, the optimiser off and on, and every export component must be what the folded
program gives exactly, as a tolerance would pass (y - x) * 1 + x for y: the inputs make that round in a lane. The
weights are powers of two, so no product rounds and a multiply fused with an add gives what the two give apart. Then
tools/needless.py finds no identity or mix pair with the optimiser on, and off only the two mixes with a 0 that the
optimiser alone sees, and the ALU instructions are what the folds leave.
"""
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

NAME = "vk_identities.frag"
X = [simulate.single(v) for v in (-1.3, 0.1, 0.7, 0.3)]
Y = [simulate.single(v) for v in (0.35, 0.7, -0.2, 0.1)]
A = [0.5, -2.0, 4.0, 0.25]
# The mix pairs and ALU instructions with the optimiser off and on.
MIX_PAIRS = {False: 2, True: 0}
ALU = {False: 24, True: 7}


def wants(x, y, a):
    """Each export as the folded program computes it, rounded to a single after each operation."""
    s = simulate.single
    weight_one, mixes, identities, arising = [], [], [], []
    for xi, yi, ai in zip(x, y, a):
        weight_one.append(yi)
        mixes.append(s(s(s(yi * ai) + s(-xi * ai + xi)) + xi))
        identities.append(s(xi - yi))
        arising.append(s(s(s(yi * ai) + xi) + yi))
    return {0: weight_one, 1: mixes, 2: identities, 3: arising}


def main():
    s = simulate.single
    if all(s(s(yi - xi) + xi) == yi for xi, yi in zip(X, Y)):
        raise SystemExit("  FAIL the inputs: (y - x) * 1 + x gives y in every lane, so the test can't tell")
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
        got = simulate.run(blob, {0: X, 1: Y, 2: A}, literals).exports
        if got != wants(X, Y, A):
            raise SystemExit(f"  FAIL {NAME}, {tag}: exports {got} where the folds give {wants(X, Y, A)}")
        counts = needless.count(blob, manifest)
        found = {"mix pairs": counts["mix_pairs"], "identities": counts["identities"] if optimize else 0,
                 "ALU": alu_count(blob)}
        want = {"mix pairs": MIX_PAIRS[optimize], "identities": 0, "ALU": ALU[optimize]}
        if found != want:
            raise SystemExit(f"  FAIL {NAME}, {tag}: {found}, not {want}")
        print(f"  PASS {NAME}, {tag}: exports exact, {found}")
    print("OK")


if __name__ == "__main__":
    main()
