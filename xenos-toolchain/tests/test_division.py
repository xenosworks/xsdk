#!/usr/bin/env python3
"""Division by constants as a multiply by the compiler's reciprocal, and by one scalar as one rcp and one vector mul
(xsdk:src/xsc_lower.c, fdiv).

vk_division.frag divides a vec3 and a float by a constant, a vector by a constant a lane, a vector by one varying
float, and lanes by varying floats and by a constant vector with a 0 in it, which keeps the GPU's rcp. Each runs on
xdis/simulate.py, the optimiser off and on, and every export component must be what the lowering gives exactly: a
times the float nearest 1 / c, which the simulator's rcp gives too, and a times rcp(b) for the rest, rcp(0) infinite.
The compiler's reciprocal is the only rounding that differs from the console's rcp (docs/HARDWARE.md, "rcp lands a
hair under a third and a fifth"), and a times it stays within an ulp of the quotient. Then tools/needless.py finds
only the two lanes by the constant with a 0 divided a lane at a time, and the ALU instructions are what that leaves.
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

NAME = "vk_division.frag"
A = [simulate.single(v) for v in (0.7, -1.3, 2.9, 0.37)]
B = [simulate.single(v) for v in (1.7, -0.6, 3.1, 2.3)]
# The divisors the shader's constants are, lane by lane.
BY_CONSTANT = [255.0, 255.0, 255.0, 7.0]
BY_CONSTANTS = [3.0, -5.0, 0.1, 256.0]
COUNTS = {"divided lanes": 2, "rcps of constants": 2}
ALU = {False: 21, True: 13}


def bits(x):
    return struct.unpack(">i", struct.pack(">f", x))[0]


def wants():
    """Each export as the lowering computes it, rounded to a single after each operation."""
    s = simulate.single

    def recip(c):
        return math.copysign(math.inf, c) if c == 0.0 else s(1.0 / s(c))

    dividends = A[:3] + B[:1]
    return {
        0: [s(a * recip(c)) for a, c in zip(dividends, BY_CONSTANT)],
        1: [s(a * recip(c)) for a, c in zip(A, BY_CONSTANTS)],
        2: [s(a * recip(B[3])) for a in A],
        3: [s(a * recip(c)) for a, c in zip(A, B[:2] + [0.0, 4.0])],
    }


def main():
    s = simulate.single
    want = wants()
    quotients = [s(a / s(c)) for a, c in zip(A[:3] + B[:1] + A, BY_CONSTANT + BY_CONSTANTS)]
    by_constants = want[0] + want[1]
    if all(q == w for q, w in zip(quotients, by_constants)):
        raise SystemExit("  FAIL the inputs: a times 1 / c rounds as a / c in every lane, so the test can't tell")
    if any(abs(bits(q) - bits(w)) > 1 for q, w in zip(quotients, by_constants)):
        raise SystemExit(f"  FAIL a times 1 / c, {by_constants}, is more than an ulp from a / c, {quotients}")
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
        got = simulate.run(blob, {0: A, 1: B}, literals).exports
        if got != want:
            raise SystemExit(f"  FAIL {NAME}, {tag}: exports {got} where the lowering gives {want}")
        counts = needless.count(blob, manifest)
        found = {"divided lanes": counts["divided_lanes"], "rcps of constants": counts["constant_rcps"],
                 "ALU": alu_count(blob)}
        expected = {**COUNTS, "ALU": ALU[optimize]}
        if found != expected:
            raise SystemExit(f"  FAIL {NAME}, {tag}: {found}, not {expected}")
        print(f"  PASS {NAME}, {tag}: exports exact, {found}")
    print("OK")


if __name__ == "__main__":
    main()
