#!/usr/bin/env python3
"""An if/else that changes a value in a pixel shader without a loop (xsdk:docs/scope/50-hlsl.md, step 8), on
xdis/simulate.py, the optimiser off and on. spirv-opt leaves HLSL's arms' values in a Phi where they meet, and glslang
leaves GLSL's one-armed change a store in that arm alone, which the fragment lowering refused (if/else arm does not
assign o_color); xost now unrolls such a shader, the arms merged by selects (xsdk:src/xsc_compile.c, lower_pixel).
Every result is the value HLSL or GLSL defines, exactly, on inputs no operation rounds.

- if_only_ps: an if without an else, c.rgb *= 0.5 where uv.x > 0.5; and its GLSL twin if_only.frag, the same values
  bit for bit.
- else_only_ps: the else alone changes c.g.
- nested_ps: nested if/else, each arm changing a component of its own.
- sampled_ps: an if on a sample's alpha, at fetch constant 1, scaling the sample.
- bool_ps: an if on a bool at register(b2), boolean constant 130, its arms in execs on it, each value of the bool.
- grid_if_ps: xsdk_test's if/else cell (xsdk:tests/test_hlsl.c), an if with no else then nested if/else on TEXCOORD0's
  x, every one of its 80 columns against the colour the CPU works out for it.

A pixel shader declares the interpolators it reads, packed from 0 by semantic: TEXCOORD0 before COLOR0 where it reads
both, COLOR0 alone at 0 in bool_ps.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

UVS = [[0.25, 0.25], [0.25, 0.75], [0.75, 0.25], [0.75, 0.75], [0.5, 0.75]]
COLOUR = [0.625, -0.375, 0.875, 0.75]
TEXEL = [0.375, 0.625, -0.25, 0.5]
SCALE_TINT = [1.5, -0.5, 2.0, 0.75]
FLIP_BOOLEAN = 130
# The grid's cell width, and COLOR0, its link colour.
GRID_WIDTH = 80
LINK = [0.25, 0.75, 0.0, 1.0]


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def literals(manifest):
    return {e[0]: list(e[1]) for e in manifest if isinstance(e[0], int)}


def if_only(uv, colour):
    c = list(colour)
    if uv[0] > 0.5:
        c[:3] = [v * 0.5 for v in c[:3]]
    return c


def else_only(uv, colour):
    c = list(colour)
    if not uv[0] > 0.5:
        c[1] = uv[1]
    return c


def nested(uv, colour):
    c = list(colour)
    if uv[0] > 0.5:
        c[0] = 1.0 - c[0]
        if uv[1] > 0.5:
            c[1] = uv[0]
        else:
            c[2] = uv[1]
    else:
        c[3] = 0.5
    return c


def grid_colour(u):
    c = [LINK[0], LINK[1], u, 1.0]
    if u > 0.25:
        c[0] += 0.5
    if u > 0.5:
        c[1] *= 0.5
        if u > 0.75:
            c[2] = 1.0 - c[2]
        else:
            c[0] = 0.0
    else:
        c[2] *= 0.5
    return c


def run(name, optimize, inputs, constants=None, fetch=None, bools=None):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
    regs = literals(manifest)
    regs.update(constants or {})
    return simulate.run(blob, inputs, regs, texture_fetch=fetch, bools=bools).exports.get(0)


def interpolated(optimize, tag):
    for name, want in (("if_only_ps.hlsl", if_only), ("if_only.frag", if_only), ("else_only_ps.hlsl", else_only),
                       ("nested_ps.hlsl", nested)):
        for uv in UVS:
            got = run(f"hlsl/if/{name}", optimize, {0: uv + [0.0, 0.0], 1: COLOUR})
            if got != want(uv, COLOUR):
                fail(f"{name}, {tag}, uv {uv}: {got}, not {want(uv, COLOUR)}")
    print(f"  PASS {tag}: if_only_ps and its GLSL twin, else_only_ps and nested_ps, every arm, as HLSL and GLSL define")


def sampled(optimize, tag):
    def fetch(slot, coords):
        if slot != 1:
            fail(f"sampled_ps, {tag}: sampled fetch constant {slot}, not 1")
        return list(TEXEL)
    for w in (0.25, 0.75):
        scale = SCALE_TINT[:3] + [w]
        got = run("hlsl/if/sampled_ps.hlsl", optimize, {0: [0.5, 0.5, 0.0, 0.0]}, {2: scale}, fetch)
        want = [t * s for t, s in zip(TEXEL[:3], scale)] + [TEXEL[3]] if TEXEL[3] < w else list(TEXEL)
        if got != want:
            fail(f"sampled_ps, {tag}, Scale.w {w}: {got}, not {want}")
    print(f"  PASS {tag}: sampled_ps scales the sample where its alpha is below Scale.w, and leaves it otherwise")


def boolean(optimize, tag):
    blob, _ = lower_ex(module("hlsl/if/bool_ps.hlsl"), XSDK_REGISTERS, optimize)
    flip = [e for e in native.constant_table(blob) if e[0] == "Flip"]
    if [(e[1], e[2]) for e in flip] != [("bool", FLIP_BOOLEAN)]:
        fail(f"bool_ps, {tag}: Flip {flip}, not boolean constant {FLIP_BOOLEAN}")
    for on in (False, True):
        got = run("hlsl/if/bool_ps.hlsl", optimize, {0: COLOUR}, {1: SCALE_TINT}, bools={FLIP_BOOLEAN} if on else set())
        g, b, r = COLOUR[1], COLOUR[2], COLOUR[0]
        want = [g * SCALE_TINT[0], b * SCALE_TINT[1], r * SCALE_TINT[2], COLOUR[3]] if on else list(COLOUR)
        if got != want:
            fail(f"bool_ps, {tag}, Flip {on}: {got}, not {want}")
    print(f"  PASS {tag}: bool_ps's Flip at boolean constant {FLIP_BOOLEAN}, both values")


def grid(optimize, tag):
    wrong = 0
    for x in range(GRID_WIDTH):
        u = simulate.single((x + 0.5) / GRID_WIDTH)
        got = run("hlsl/grid_if_ps.hlsl", optimize, {0: [u, 0.0, 0.0, 0.0], 1: LINK})
        want = grid_colour(u)
        if any(abs(a - b) > 1e-6 for a, b in zip(got, want)):
            wrong += 1
            if wrong == 1:
                print(f"  column {x}: {got}, not {want}")
    if wrong:
        fail(f"grid_if_ps, {tag}: {wrong} of {GRID_WIDTH} columns wrong")
    print(f"  PASS {tag}: grid_if_ps's {GRID_WIDTH} columns, as the CPU works them out")


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        interpolated(optimize, tag)
        sampled(optimize, tag)
        boolean(optimize, tag)
        grid(optimize, tag)
    print("OK")


if __name__ == "__main__":
    main()
