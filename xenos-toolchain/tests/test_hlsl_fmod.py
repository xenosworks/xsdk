#!/usr/bin/env python3
"""HLSL's fmod as glslang-xenon writes it (xsdk:docs/scope/50-hlsl.md, step 7), on xdis/simulate.py, the optimiser off
and on. glslang made fmod GLSL's mod, OpFMod, which floors the quotient and takes the sign of y; HLSL's truncates it,
x - y * trunc(x / y), the sign of x (glslang-xenon 40a553d5).

- fmod_ps: a vector's lanes from TEXCOORD0 and TEXCOORD1, x and y negative among them, and scalars by a constant y,
  folded constants and a register's y, each result exact, the C library's truncating fmod's.
- i05_math_ps, the idiom: m = fmod(uv.y * 7.0, 2.0) is -0.5 at uv.y * 7.0 = -2.5, so step(1.0, m) is 0 and so is the
  alpha the mix by it gives, where the floor's 1.5 made it half; at +2.5 both are 0.
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

# (x, y, Y.w): each quotient far enough from a whole number that the reciprocal's rounding can't move its trunc.
TRIALS = [([-7.5, 7.5, -7.5, 5.25], [2.0, -2.0, -2.0, -1.5], -1.5),
          ([-0.5, 6.5, 3.25, -5.25], [6.5, -0.75, 1.0, 2.5], 2.5)]


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def literals(manifest):
    return {e[0]: list(e[1]) for e in manifest if isinstance(e[0], int)}


def fmod_case(optimize, tag):
    blob, manifest = lower_ex(module("hlsl/fmod/fmod_ps.hlsl"), XSDK_REGISTERS, optimize)
    for x, y, w in TRIALS:
        regs = literals(manifest)
        regs[0] = [0.0, 0.0, 0.0, w]
        got = simulate.run(blob, {0: x, 1: y}, regs).exports
        want = [[math.fmod(a, b) for a, b in zip(x, y)],
                [math.fmod(x[1], -2.0), math.fmod(-7.5, 2.0), math.fmod(7.5, -2.0), math.fmod(x[3], w)]]
        if [got.get(0), got.get(1)] != want:
            fail(f"fmod_ps, {tag}, x {x}, y {y}: lanes {got.get(0)} and scalars {got.get(1)}, not {want[0]} and "
                 f"{want[1]}")
    print(f"  PASS {tag}: fmod_ps's lanes and scalars truncate, the sign of x, as HLSL's fmod")


def idiom(optimize, tag):
    blob, manifest = lower_ex(module("hlsl/idioms/i05_math_ps.hlsl"), XSDK_REGISTERS, optimize)
    regs = literals(manifest)
    # Threshold below the texel's alpha, so clip keeps the pixel; ColorA black and ColorB white.
    regs.update({0: [0.5, 0.0, 0.0, 0.0], 1: [0.0] * 4, 2: [1.0] * 4})
    for scaled in (-2.5, 2.5):
        got = simulate.run(blob, {0: [0.05, scaled / 7.0, 0.0, 0.0], 1: [1.0] * 4}, regs,
                           texture_fetch=lambda slot, coords: [1.0] * 4)
        alpha = (got.exports.get(0) or [None] * 4)[3]
        if got.killed or alpha != 0.0:
            fail(f"i05_math_ps, {tag}, uv.y * 7.0 = {scaled}: alpha {alpha}, killed {got.killed}, not 0.0, fmod's "
                 "step(1.0, m) 0")
    print(f"  PASS {tag}: i05_math_ps's fmod(-2.5, 2.0) is -0.5, its step 0")


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        fmod_case(optimize, tag)
        idiom(optimize, tag)
    print("OK")


if __name__ == "__main__":
    main()
