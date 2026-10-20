#!/usr/bin/env python3
"""Discards beside other control flow (xsdk:docs/scope/50-hlsl.md, step 8), on xdis/simulate.py, the optimiser off and
on. HLSL's clip, discard and texkill may sit anywhere in a pixel shader, and glslang makes clip an if around OpKill.
The fragment lowering's one kill-if took a discard alone: beside another if/else it killed on its own if alone, or
refused. Such a shader now goes through the unroller, which defers each discard: a pixel that takes one is killed once
where the function ends, by an if on what it accumulated around one OpKill, the kill-if's, and runs on till then for
its quad's derivatives (xsdk:src/xsc_unroll.c, discard). Each pixel is killed exactly where HLSL or GLSL kills it, and
each that lives has exactly the colour they define, on inputs no operation rounds.

- clip_if_ps: a clip, then an if; if_clip_ps: an if, then a clip on the value it changed; if_discard.frag: a discard
  under an if, then an if.
- nested.frag and its HLSL twin nested_ps: a discard after a return in an if inside another, which the kill-if alone
  compiled killing on the outer if; inner_ps: a discard in an if inside another, values changed before and after it.
- beside_return_ps: a discard beside an early return; after_return_ps: after a return in an unrolled loop; and
  after_return.frag: after one the unroller takes as a local is stored again.
- vector_ps: a clip on a float4, which kills where any component is below 0, then an if; vector_only_ps: one alone,
  the kill-if's.
- sampled_ps: a clip on a sample's alpha at fetch constant 1, then an if and a second sample at a coordinate worked out
  after it, run as 2x2 quads, the right half of one killed: every live pixel's second LOD has the whole quad's
  gradients.
- loop_ps: a discard in an unrolled loop, beside a break; nested_loop_ps: in an unrolled loop inside another, past the
  outer loop's break.
- grid_discard_ps: xsdk_test's discard cell (xsdk:tests/test_hlsl.c), every one of its 80 columns.
- Refused: refused_counted_ps, a discard inside a hardware loop.

A pixel shader declares the interpolators it reads, packed from 0 by semantic: TEXCOORD0 before COLOR0.
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

POINTS = [0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875]
UVS = [[x, y] for x in POINTS for y in POINTS]
COLOUR = [0.625, -0.375, 0.875, 0.75]
# The loops' colour, red low enough that the passes' reds cross the points.
LOOP_COLOUR = [0.125, -0.375, 0.875, 0.75]
# sampled_ps's quads: top left corners, a pixel apart, the second's right half killed by its alpha.
QUAD_CORNERS = [[0.3125, 0.25], [0.4375, 0.25]]
PIXEL = 0.125
# The grid's cell width, and COLOR0, its link colour.
GRID_WIDTH = 80
LINK = [0.25, 0.75, 0.0, 1.0]
REFUSALS = {"discard/refused_counted_ps.hlsl": "a discard inside a hardware loop"}


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def literals(manifest):
    return {e[0]: list(e[1]) for e in manifest if isinstance(e[0], int)}


def program(name, optimize):
    blob, manifest = lower_ex(module(f"hlsl/{name}"), XSDK_REGISTERS, optimize)
    return blob, literals(manifest)


def outcome(result):
    """None for a killed pixel, else its colour."""
    return None if result.killed else result.exports.get(0)


def scaled(v, k):
    return [x * k for x in v]


def clip_if(uv, colour):
    if uv[1] - 0.25 < 0.0:
        return None
    c = [uv[0], uv[1], 0.25, 1.0]
    return scaled(c[:3], 0.5) + c[3:] if uv[0] > 0.5 else c


def if_clip(uv, colour):
    c = list(colour)
    if uv[0] > 0.5:
        c[1] = uv[1]
    if c[1] - 0.5 < 0.0:
        return None
    c[2] = uv[0]
    return c


def if_discard(uv, colour):
    if uv[0] > 0.5:
        return None
    return [1.0 if uv[1] > 0.5 else uv[0], uv[1], 0.0, 1.0]


def nested(uv, colour):
    return [uv[0], uv[1], 0.0, 1.0] if uv[1] > 0.25 and uv[0] > 0.5 else None


def inner(uv, colour):
    c = list(colour)
    if uv[0] > 0.5:
        c[0] = 1.0 - c[0]
        if uv[1] < 0.5:
            return None
        c[2] = uv[1]
    c[1] = uv[0]
    return c


def beside_return(uv, colour):
    if uv[0] > 0.75:
        return [1.0, 0.0, uv[1], 1.0]
    if uv[1] < 0.5:
        return None
    return [colour[0], uv[0]] + colour[2:]


def after_return_loop(uv, colour):
    c = list(colour)
    for _ in range(4):
        c[0] += 0.125
        if c[0] > uv[0]:
            return c
    return None


def after_return(uv, colour):
    return [colour[0], uv[1]] + colour[2:] if uv[0] > 0.5 else None


def vector_clip(uv):
    return any(d < 0.0 for d in (uv[0] - 0.25, uv[1] - 0.25, 0.75 - uv[0], 0.75 - uv[1]))


def vector(uv, colour):
    return None if vector_clip(uv) else [uv[1] if uv[0] > 0.5 else colour[0]] + colour[1:]


def vector_only(uv, colour):
    return None if vector_clip(uv) else list(colour)


def loop(uv, colour):
    c = list(colour)
    for _ in range(4):
        c[0] += 0.125
        if c[0] > uv[1]:
            break
        if c[0] > uv[0]:
            return None
    c[1] = uv[1]
    return c


def nested_loop(uv, colour):
    c = list(colour)
    for _ in range(3):
        c[0] += 0.125
        if c[0] > uv[1]:
            break
        for _ in range(2):
            c[2] += 0.125
            if c[2] > uv[0] + 1.0:
                return None
    return c


def grid_discard(u):
    if u - 0.125 < 0.0:
        return None
    c = [LINK[0], LINK[1], u, 1.0]
    if u > 0.5:
        c[0] += 0.5
        if u < 0.75:
            return None
        c[2] = 1.0 - c[2]
    c[1] *= 0.5
    return c


def interpolated(optimize, tag):
    cases = (("clip_if_ps.hlsl", clip_if, COLOUR), ("if_clip_ps.hlsl", if_clip, COLOUR),
             ("if_discard.frag", if_discard, COLOUR), ("nested.frag", nested, COLOUR),
             ("nested_ps.hlsl", nested, COLOUR), ("inner_ps.hlsl", inner, COLOUR),
             ("beside_return_ps.hlsl", beside_return, COLOUR), ("after_return_ps.hlsl", after_return_loop, LOOP_COLOUR),
             ("after_return.frag", after_return, COLOUR), ("vector_ps.hlsl", vector, COLOUR),
             ("vector_only_ps.hlsl", vector_only, COLOUR), ("loop_ps.hlsl", loop, LOOP_COLOUR),
             ("nested_loop_ps.hlsl", nested_loop, LOOP_COLOUR))
    for name, want, colour in cases:
        blob, constants = program(f"discard/{name}", optimize)
        killed = 0
        for uv in UVS:
            got = outcome(simulate.run(blob, {0: uv + [0.0, 0.0], 1: colour}, constants))
            if got != want(uv, colour):
                fail(f"{name}, {tag}, uv {uv}: {'killed' if got is None else got}, not "
                     f"{'killed' if want(uv, colour) is None else want(uv, colour)}")
            killed += got is None
        if not killed or killed == len(UVS):
            fail(f"{name}, {tag}: {killed} of {len(UVS)} killed, which tells nothing")
    print(f"  PASS {tag}: clip_if_ps, if_clip_ps, if_discard.frag, nested.frag and nested_ps, inner_ps, "
          "beside_return_ps, after_return_ps and after_return.frag, vector_ps and vector_only_ps, and loop_ps and "
          f"nested_loop_ps kill exactly the pixels HLSL and GLSL kill at {len(UVS)} points, and give each pixel that "
          "lives its colour")


def texel(coords, lod):
    """The coordinates, the LOD's horizontal gradient of the first, and alpha 0.75 left of 0.5, 0.25 from it."""
    return [coords[0], coords[1], lod.gradients[0][0], 0.75 if coords[0] < 0.5 else 0.25]


def sampled(optimize, tag):
    blob, constants = program("discard/sampled_ps.hlsl", optimize)

    def fetch(slot, coords, lod):
        if slot != 1 or not lod.computed or not lod.gradients:
            fail(f"sampled_ps, {tag}: a sample at fetch constant {slot}, its LOD {lod}, not 1's, computed")
        return texel(coords, lod)
    mixed = 0
    for x, y in QUAD_CORNERS:
        uvs = [[x, y], [x + PIXEL, y], [x, y + PIXEL], [x + PIXEL, y + PIXEL]]
        results = simulate.run_group(blob, [{0: uv + [0.0, 0.0], 1: COLOUR} for uv in uvs], constants,
                                     texture_lod=fetch, quads=True)
        for uv, result in zip(uvs, results):
            t = texel(uv, simulate.Lod(True, 0.0, None, ([PIXEL, 0.0, 0.0], [0.0, PIXEL, 0.0])))
            if uv[0] > 0.5:
                t = [a * b for a, b in zip(t[:3], COLOUR[:3])] + t[3:]
            p = [2.0 * v for v in uv]
            s = texel(p, simulate.Lod(True, 0.0, None, ([2.0 * PIXEL, 0.0, 0.0], [0.0, 2.0 * PIXEL, 0.0])))
            want = None if t[3] - 0.5 < 0.0 else [a * b for a, b in zip(s, t)]
            if outcome(result) != want:
                fail(f"sampled_ps, {tag}, uv {uv}: {outcome(result)}, not {want}")
        mixed += 0 < sum(r.killed for r in results) < len(results)
    if not mixed:
        fail(f"sampled_ps, {tag}: no quad with a killed pixel beside a live one")
    print(f"  PASS {tag}: sampled_ps kills where its sample's alpha is below 0.5, and a quad with killed pixels gives "
          "its live ones the second sample's whole gradients, twice the first's")


def grid(optimize, tag):
    blob, constants = program("grid_discard_ps.hlsl", optimize)
    wrong = killed = 0
    for x in range(GRID_WIDTH):
        u = simulate.single((x + 0.5) / GRID_WIDTH)
        got = outcome(simulate.run(blob, {0: [u, 0.0, 0.0, 0.0], 1: LINK}, constants))
        want = grid_discard(u)
        killed += got is None
        if (got is None) != (want is None) or (got and any(abs(a - b) > 1e-6 for a, b in zip(got, want))):
            wrong += 1
            if wrong == 1:
                print(f"  grid_discard_ps column {x}: {got}, not {want}")
    if wrong:
        fail(f"grid_discard_ps.hlsl, {tag}: {wrong} of {GRID_WIDTH} columns wrong")
    print(f"  PASS {tag}: grid_discard_ps's {GRID_WIDTH} columns, {killed} killed, as the CPU works them out")


def refused():
    for name, why in REFUSALS.items():
        try:
            native.compile_spirv(module(f"hlsl/{name}").blob, XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, True)
        except native.CompileError as e:
            if e.status != native.REFUSED or why not in str(e):
                fail(f"{name}: {e.status}, {e}")
        else:
            fail(f"{name} compiled")
        print(f"  PASS {name}: refused, {why}")


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        interpolated(optimize, tag)
        sampled(optimize, tag)
        grid(optimize, tag)
    refused()
    print("OK")


if __name__ == "__main__":
    main()
