#!/usr/bin/env python3
"""Early returns and switches (xsdk:docs/scope/50-hlsl.md, step 8), on xdis/simulate.py, the optimiser off and on.
glslang runs spirv-opt's legalisation on HLSL, whose merge-return wraps a shader with an early return in a switch on a
constant with no cases, each return a branch to its merge; xost copies that merge to each branch, so each returns where
it is, as GLSL's does, and a return on data the unroller now takes, each output keeping what it held where its pixel or
vertex returned (xsdk:src/xsc_switch.c, src/xsc_unroll.c, return_on_data). A switch becomes an if/else chain. Every
result is the value HLSL or GLSL defines, exactly, on inputs no operation rounds.

- return/if_ps: an early return under an if; and its GLSL twin if.frag, the same values bit for bit.
- return/else_ps: the return in the else; nested_ps: in an if inside another; two_ps: two of them.
- return/sampled_ps: after a sample at fetch constant 1, on its alpha.
- return/vertex_vs: in a vertex shader, the position set before it and COLOR0 by it or after it.
- return/loop_ps: inside a loop, which spirv-opt makes a flag and a break, unrolled; counted_ps: inside a hardware
  loop an int at register(i0), loop constant 16, counts, at counts 0, 2 and 5.
- switch/switch_ps: cases that break, two literals sharing one, and a default, at every case; its GLSL twin switch.frag,
  which xost compiled before as if the switch weren't there; switch_vs: a vertex shader's, without a default; return_ps:
  an early return in a case.
- grid_return_ps and grid_switch_ps: xsdk_test's cells (xsdk:tests/test_hlsl.c), every one of their 80 columns.
- Refused: switch/refused_fallthrough_ps, a case falling through to the next; return/refused_discard_ps and its GLSL
  twin, a discard after a return on data, which the unroller emits no kill for.

A pixel shader declares the interpolators it reads, packed from 0 by semantic: TEXCOORD0 before COLOR0.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from test_hlsl_rest import exports_by_name, program, run_vertex  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

UVS = [[0.25, 0.25], [0.25, 0.75], [0.75, 0.25], [0.75, 0.75], [0.875, 0.375], [0.5, 0.75]]
COLOUR = [0.625, -0.375, 0.875, 0.75]
TEXEL = [0.375, 0.625, -0.25, 0.5]
TINT = [1.5, -0.5, 2.0, 0.75]
SCALE = [2.0, 0.5, -1.0, 1.0]
POSITIONS = [[0.25, -0.5, 0.75, 1.0], [0.75, 0.5, -0.25, 1.0], [0.5, 0.25, 0.5, 1.0], [0.625, 0.125, 0.0, 1.0]]
COUNT_LOOP = 16
# The grid's cell width, and COLOR0, its link colour.
GRID_WIDTH = 80
LINK = [0.25, 0.75, 0.0, 1.0]
REFUSALS = {"switch/refused_fallthrough_ps.hlsl": "a switch's case falling through to another",
            "return/refused_discard_ps.hlsl": "a discard after a return on data",
            "return/refused_discard.frag": "a discard after a return on data"}


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def literals(manifest):
    return {e[0]: list(e[1]) for e in manifest if isinstance(e[0], int)}


def run(name, optimize, inputs, constants=None, fetch=None, loops=None):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
    regs = literals(manifest)
    regs.update(constants or {})
    return simulate.run(blob, inputs, regs, texture_fetch=fetch, loops=loops).exports.get(0)


def scaled(v, k):
    return [x * k for x in v]


def if_return(uv, colour):
    if uv[0] > 0.5:
        return [uv[0], uv[1], 0.25, 1.0]
    return [colour[0], uv[1], colour[2], colour[3]]


def else_return(uv, colour):
    if not uv[0] > 0.5:
        return [uv[0], uv[1], 0.75, 1.0]
    return scaled(colour[:3], 0.5) + [uv[1]]


def nested(uv, colour):
    c = list(colour)
    if uv[0] > 0.5:
        c[0] = 1.0 - c[0]
        if uv[1] > 0.5:
            return scaled(c, 0.5)
        c[2] = uv[1]
    c[1] = uv[0]
    return c


def two(uv, colour):
    c = list(colour)
    if uv[0] > 0.75:
        return [1.0, 0.0, 0.25, 1.0]
    c[1] = uv[1]
    if uv[1] > 0.5:
        return scaled(c, 0.5)
    c[2] = uv[0]
    return c


def loop(uv, colour, count=4, flat=False):
    """loop_ps, and counted_ps where flat, whose return's green is 0.25 whatever the pass."""
    c = list(colour)
    for i in range(count):
        c[0] += 0.125
        if c[0] > uv[0]:
            return [c[0], 0.25 if flat else i * 0.25, 0.5, 1.0]
    c[1] = uv[1]
    return c


def switch(uv, colour):
    c = list(colour)
    k = int(uv[0] * 8.0)
    if k == 0:
        c[0] = 0.25
    elif k in (2, 3):
        c[1] = uv[1]
    elif k == 5:
        c[2], c[3] = 0.75, 0.5
    else:
        c = scaled(c, 0.5)
    return c


def switch_return(uv, colour):
    c = list(colour)
    k = int(uv[0] * 4.0)
    if k == 0:
        return [1.0, 0.0, 0.25, 1.0]
    if k == 1:
        c[1] = uv[1]
    else:
        c[2] = 0.75
    c[3] = 0.5
    return c


def grid_return(u):
    c = [LINK[0], LINK[1], u, 1.0]
    if u > 0.75:
        return [1.0, 0.0, 1.0 - u, 1.0]
    c[0] += 0.5
    if u > 0.25:
        c[1] *= 0.5
        if u < 0.5:
            return c
        c[2] = 1.0 - c[2]
    c[0] *= 0.5
    return c


def grid_switch(u):
    c = [LINK[0], LINK[1], u, 1.0]
    k = int(u * 5.0)
    if k == 0:
        c[0] = 1.0
    elif k in (1, 3):
        c[1] *= 0.5
        c[2] = 1.0 - c[2]
    elif k == 2:
        c[0], c[1] = c[1], c[0]
    else:
        c[2] *= 0.5
    return c


def interpolated(optimize, tag):
    cases = (("return/if_ps.hlsl", if_return), ("return/if.frag", if_return), ("return/else_ps.hlsl", else_return),
             ("return/nested_ps.hlsl", nested), ("return/two_ps.hlsl", two),
             ("switch/return_ps.hlsl", switch_return))
    for name, want in cases:
        for uv in UVS + [[0.125, 0.5], [0.375, 0.25]]:
            got = run(f"hlsl/{name}", optimize, {0: uv + [0.0, 0.0], 1: COLOUR})
            if got != want(uv, COLOUR):
                fail(f"{name}, {tag}, uv {uv}: {got}, not {want(uv, COLOUR)}")
    print(f"  PASS {tag}: if_ps and its GLSL twin, else_ps, nested_ps, two_ps and switch/return_ps, every return and "
          "every path past them, as HLSL and GLSL define")


def sampled(optimize, tag):
    def fetch(slot, coords):
        if slot != 1:
            fail(f"sampled_ps, {tag}: sampled fetch constant {slot}, not 1")
        return list(TEXEL)
    for w in (0.25, 0.75):
        tint = TINT[:3] + [w]
        uv = [0.25, 0.5]
        got = run("hlsl/return/sampled_ps.hlsl", optimize, {0: uv + [0.0, 0.0]}, {2: tint}, fetch)
        want = ([t * k for t, k in zip(TEXEL, tint)] if TEXEL[3] < w else
                [t * 0.5 + uv[0] for t in TEXEL[:3]] + [TEXEL[3]])
        if got != want:
            fail(f"sampled_ps, {tag}, Tint.w {w}: {got}, not {want}")
    print(f"  PASS {tag}: sampled_ps returns the tinted sample where its alpha is below Tint.w, and goes on otherwise")


def loops(optimize, tag):
    colour = [0.125] + COLOUR[1:]
    for x in (0.0, 0.2, 0.45, 0.6, 0.7):
        uv = [x, 0.375]
        got = run("hlsl/return/loop_ps.hlsl", optimize, {0: uv + [0.0, 0.0], 1: colour})
        if got != loop(uv, colour):
            fail(f"loop_ps, {tag}, uv {uv}: {got}, not {loop(uv, colour)}")
        for count in (0, 2, 5):
            got = run("hlsl/return/counted_ps.hlsl", optimize, {0: uv + [0.0, 0.0], 1: colour},
                      loops={COUNT_LOOP: count})
            want = loop(uv, colour, count, flat=True)
            if got != want:
                fail(f"counted_ps, {tag}, uv {uv}, Count {count}: {got}, not {want}")
    print(f"  PASS {tag}: loop_ps returns from each pass of its unrolled loop, and counted_ps from its hardware loop's "
          f"at loop constant {COUNT_LOOP}, or goes past either")


def switches(optimize, tag):
    for k in range(8):
        uv = [(k + 0.5) / 8.0, 0.625]
        want = switch(uv, COLOUR)
        for name in ("switch_ps.hlsl", "switch.frag"):
            got = run(f"hlsl/switch/{name}", optimize, {0: uv + [0.0, 0.0], 1: COLOUR})
            if got != want:
                fail(f"{name}, {tag}, int {k}: {got}, not {want}")
    print(f"  PASS {tag}: switch_ps and its GLSL twin take each case, both literals of the shared one and the default")


def vertices(optimize, tag):
    for position in POSITIONS:
        m, blob, manifest = program("hlsl/return/vertex_vs.hlsl", optimize)
        got = exports_by_name(m, run_vertex(blob, m, manifest, {"position": position, "colour": COLOUR}, [SCALE]))
        colour = [1.0, 0.0, 0.25, 1.0] if position[0] > 0.5 else [COLOUR[0], COLOUR[1] * 0.5] + COLOUR[2:]
        want = {"position": [p * s for p, s in zip(position, SCALE)], "colour": colour}
        if {k: got[k] for k in want} != want:
            fail(f"vertex_vs, {tag}, position {position}: {got}, not {want}")
        m, blob, manifest = program("hlsl/switch/switch_vs.hlsl", optimize)
        got = exports_by_name(m, run_vertex(blob, m, manifest, {"position": position, "colour": COLOUR}, []))
        k = int(position[0] * 4.0)
        colour = [0.25] + COLOUR[1:] if k == 1 else [COLOUR[0], 0.5] + COLOUR[2:] if k == 2 else list(COLOUR)
        if got["position"] != position or got["colour"] != colour:
            fail(f"switch_vs, {tag}, position {position}: {got}, not {position} and {colour}")
    print(f"  PASS {tag}: vertex_vs's early return and switch_vs's cases, as HLSL defines them")


def grid(optimize, tag):
    for name, want in (("grid_return_ps.hlsl", grid_return), ("grid_switch_ps.hlsl", grid_switch)):
        wrong = 0
        for x in range(GRID_WIDTH):
            u = simulate.single((x + 0.5) / GRID_WIDTH)
            got = run(f"hlsl/{name}", optimize, {0: [u, 0.0, 0.0, 0.0], 1: LINK})
            if any(abs(a - b) > 1e-6 for a, b in zip(got, want(u))):
                wrong += 1
                if wrong == 1:
                    print(f"  {name} column {x}: {got}, not {want(u)}")
        if wrong:
            fail(f"{name}, {tag}: {wrong} of {GRID_WIDTH} columns wrong")
    print(f"  PASS {tag}: grid_return_ps's and grid_switch_ps's {GRID_WIDTH} columns, as the CPU works them out")


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
        loops(optimize, tag)
        switches(optimize, tag)
        vertices(optimize, tag)
        grid(optimize, tag)
    refused()
    print("OK")


if __name__ == "__main__":
    main()
