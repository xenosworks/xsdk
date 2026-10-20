#!/usr/bin/env python3
"""DX9's b# and i# registers, which glslang-xenon keeps as a $Global member's UserSemantic, "register(b3)" or
"register(i2)" (xsdk:docs/scope/50-hlsl.md, step 7; glslang-xenon 026e7416), on xdis/simulate.py, the optimiser off and
on: a bool an if reads is the boolean constant D3D9's b# maps to, 128 + N in a pixel shader, libxenon's pixel half
(libxenon:drivers/xenos/xe.c:2038-2064), and an int a loop counts the loop constant its i# maps to, 16 + N in a pixel
shader and N in a vertex shader (xsdk:src/xsc_lower_flow.c); one without a register takes the lowest no register
names.

- registers_ps: Low at b0 is 128; Tint at b1, a select's value, a float register of its own, keeps 129 free; Plain,
  without a register, 130; Outer at i0 is 16, and Inner, without one, 17. Each value of each bool, and counts, drawn
  with the constants set where D3D9 puts them, as the table names them.
- registers_vs: Count at i2 is loop constant 2; Flag at b1 a value, as a vertex shader's bools are.
- Refused: refused_same_ps, two bools at b2; refused_type_ps, an int at b0; refused_past_ps, a bool an if reads at b130,
  past the pixel shader's 128.
"""
import itertools
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

# The tables' classes and types (XenosRecomp:XenosRecomp/constant_table.h): scalar 0; bool 1, int 2.
SCALAR, BOOL, INT = 0, 1, 2
PIXEL_BOOLEAN, PIXEL_LOOP = 128, 16
UV = [0.5, -1.25, 2.0, 0.75]
STEP = [0.25, -0.5, 1.5, 0.125]
POSITION = [1.5, -0.25, 0.75, 1.0]


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def literals(manifest):
    return {e[0]: list(e[1]) for e in manifest if isinstance(e[0], int)}


def compiled(name, optimize):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
    return blob, manifest, native.constant_table(blob)


def entry(table, name, register_set):
    found = [e for e in table if e[0] == name and e[1] == register_set]
    if len(found) != 1:
        fail(f"{name} in the {register_set} set: {found}")
    return found[0]


def pixel(inner, outer, plain, low, tint):
    c = list(UV)
    c[0] += STEP[3] * inner
    c = [x + k * outer for x, k in zip(c, STEP)]
    if plain:
        c = [2.0 * c[i] for i in (1, 0, 3, 2)]
    if low:
        c = [x * 0.5 + k for x, k in zip(c, STEP)]
    return [x + (k if tint else 0.0) for x, k in zip(c, STEP)]


def registers_ps(optimize, tag):
    blob, manifest, table = compiled("hlsl/registers/registers_ps.hlsl", optimize)
    places = {n: entry(table, n, s)[2:] for n, s in (("Low", "bool"), ("Plain", "bool"), ("Outer", "int4"),
                                                      ("Inner", "int4"))}
    want = {"Low": (PIXEL_BOOLEAN, 1, SCALAR, BOOL, 1, 1, 1), "Plain": (PIXEL_BOOLEAN + 2, 1, SCALAR, BOOL, 1, 1, 1),
            "Outer": (PIXEL_LOOP, 1, SCALAR, INT, 1, 1, 1), "Inner": (PIXEL_LOOP + 1, 1, SCALAR, INT, 1, 1, 1)}
    if places != want:
        fail(f"registers_ps, {tag}: {places}, not Low at boolean 128, Plain 130, Outer at loop constant 16, Inner 17")
    tint = entry(table, "Tint", "float4")
    if [e for e in table if e[0] == "Tint" and e[1] != "float4"]:
        fail(f"registers_ps, {tag}: Tint, a value, at a boolean constant: {table}")
    for inner, outer, plain, low, tints in itertools.product((0, 3), (0, 2), (False, True), (False, True), (0, 1)):
        regs = literals(manifest)
        regs.update({0: STEP, tint[2]: [float(tints)] * 4})
        bools = {PIXEL_BOOLEAN + 2} if plain else set()
        bools |= {PIXEL_BOOLEAN} if low else set()
        got = simulate.run(blob, {0: UV}, regs, loops={PIXEL_LOOP: outer, PIXEL_LOOP + 1: inner}, bools=bools)
        want_colour = pixel(inner, outer, plain, low, tints)
        if got.exports.get(0) != want_colour:
            fail(f"registers_ps, {tag}, Inner {inner}, Outer {outer}, Plain {plain}, Low {low}, Tint {tints}: "
                 f"{got.exports.get(0)}, not {want_colour}")
    print(f"  PASS {tag}: registers_ps's Low at boolean 128, Plain 130, Outer at loop constant 16, Inner 17, Tint at "
          f"c{tint[2]}, each value right")


def registers_vs(optimize, tag):
    blob, manifest, table = compiled("hlsl/registers/registers_vs.hlsl", optimize)
    count = entry(table, "Count", "int4")
    flag = entry(table, "Flag", "float4")
    if count[2:] != (2, 1, SCALAR, INT, 1, 1, 1):
        fail(f"registers_vs, {tag}: Count {count}, not loop constant 2")
    for passes, flags in itertools.product((0, 3), (0, 1)):
        regs = literals(manifest)
        regs.update({0: STEP, flag[2]: [float(flags)] * 4})
        got = simulate.run(blob, {0: [0.0] * 4}, regs, vertex_fetch=lambda n, fetch, index: POSITION,
                           loops={2: passes})
        p = [x + k * passes for x, k in zip(POSITION, STEP)]
        want = [2.0 * p[i] for i in (1, 0, 3, 2)] if flags else p
        if got.exports.get(62) != want:
            fail(f"registers_vs, {tag}, Count {passes}, Flag {flags}: {got.exports.get(62)}, not {want}")
    print(f"  PASS {tag}: registers_vs's Count at loop constant 2, Flag at c{flag[2]}, each value right")


REFUSED = [("refused_same_ps", "A and B share register(b2)"),
           ("refused_type_ps", "Count at register(b0): a b# register holds a bool, an i# an int"),
           ("refused_past_ps", "Far at register(b130), past the stage's 128")]


def refused():
    for name, message in REFUSED:
        try:
            native.compile_spirv(module(f"hlsl/registers/{name}.hlsl").blob, XSDK_REGISTERS.pixel,
                                 XSDK_REGISTERS.vertex, True)
        except native.CompileError as e:
            if e.status != native.REFUSED or message not in str(e):
                fail(f"{name}: {e.status}, {e}")
        else:
            fail(f"{name} compiled")
        print(f"  PASS {name}: refused, {message}")


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        registers_ps(optimize, tag)
        registers_vs(optimize, tag)
    refused()
    print("OK")


if __name__ == "__main__":
    main()
