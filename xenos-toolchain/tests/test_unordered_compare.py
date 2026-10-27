#!/usr/bin/env python3
"""xsc lowers a float comparison's unordered form as its ordered one (xsdk:src/xsc_spirv.c): glslang makes GLSL's !=
on floats OpFUnordNotEqual, which PPSSPP's alpha test uses (God of War: Chains of Olympus), and xost failed on it, the
comparison's result made by no instruction it read. They differ for NaN alone.

- vk_unordered_compare.frag: a discard where round(a * 255) != the block's alpha, and colours from two more !=, on the
  simulator, the optimiser off and on: a pixel lives where its alpha rounds to the reference, each colour 1.0 where its
  channel differs from the reference's and 0.25 where it's equal. XenosRecomp's oracle accepts it.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

NAME = "vk_unordered_compare.frag"
REFERENCE = [0.5, 0.25, 0.0, 128.0]
# (colour, killed): alphas rounding to 128 and not, channels equal to the reference's and not.
CASES = [
    ([0.5, 0.25, 0.0, 128.0 / 255.0], False),
    ([0.5, 0.75, 0.0, 128.2 / 255.0], False),
    ([0.125, 0.25, 0.0, 127.9 / 255.0], False),
    ([0.125, 0.75, 0.0, 128.0 / 255.0], False),
    ([0.5, 0.25, 0.0, 127.0 / 255.0], True),
    ([0.5, 0.25, 0.0, 129.0 / 255.0], True),
    ([0.5, 0.25, 0.0, 0.0], True),
]


def want(colour):
    return [1.0 if colour[0] != REFERENCE[0] else 0.25, 1.0 if colour[1] != REFERENCE[1] else 0.25, 0.0, 1.0]


def check(optimize):
    blob, manifest, resources = native.compile_spirv((TD / f"{NAME}.spv").read_bytes(), XSDK_REGISTERS.pixel,
                                                     XSDK_REGISTERS.vertex, optimize, True)
    mode = f"{NAME}, optimiser {'on' if optimize else 'off'}"
    block = next(r for r in resources if r[0] == "uniform")
    constants = {reg: list(values) for reg, values in (n for n in manifest if isinstance(n[0], int))}
    constants[block[3]] = REFERENCE
    for colour, killed in CASES:
        result = simulate.run(blob, {0: [simulate.single(c) for c in colour]}, constants)
        got = None if result.killed else result.exports.get(0)
        if killed != (got is None) or (got is not None and not all(
                math.isclose(a, b, abs_tol=1e-6) for a, b in zip(got, want(colour)))):
            print(f"  FAIL {mode}: colour {colour}: {'killed' if got is None else got}, not "
                  f"{'killed' if killed else want(colour)}")
            return False
    if oracle_hlsl(blob, f"unordered_compare_{int(optimize)}") is None:
        print(f"  FAIL {mode}: XenosRecomp rejected it")
        return False
    print(f"  PASS {mode}: {len(CASES)} pixels killed or coloured as != says; the oracle accepts it")
    return True


def main():
    try:
        results = [check(False), check(True)]
    except native.CompileError as error:
        print(f"  FAIL {NAME}: {native.STATUS_NAMES[error.status]}: {error}")
        results = [False]
    if not all(results):
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
