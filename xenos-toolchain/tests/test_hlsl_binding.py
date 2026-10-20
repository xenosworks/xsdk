#!/usr/bin/env python3
"""Binding as D3D9 binds (xsdk:docs/scope/50-hlsl.md, step 6), on xdis/simulate.py, the optimiser off and on.

- slots_ps: samplers at s1 and s4 sample fetch constants 1 and 4, as D3D9's SetTexture(1) and (4) bind them; the two
  without a register 0 and 2, the lowest no register names; its GLSL twin slots.frag, at Vulkan bindings 1, 4, 0 and 2,
  still packs from 0 in binding order.
- integers_ps: glslang packs all four of its bools and ints into c2. Flip at b3, which an if reads, is boolean constant
  131, 128 + 3; Count at i2, which a loop counts, loop constant 18, 16 + 2 (step 7); Tint at b1 and Scale at i0,
  read as values, floats in registers of their own past the block, c2 holding something else. Each value of Flip and
  Tint, Count 0 and 2.
- unrolled_ps: an if on a bool past an unrolled loop, no hardware loop left, both values of the bool: the optimiser
  leaves alone a program with execs on a boolean constant, whose arms both write the register read after them.
- table_ps: every entry of its table, register set, index and count, class, type, rows, columns and elements.
- grid_binding_ps: xsdk_test's binding cell, its two halves drawn as tests/test_hlsl.c sets them, Low and High bound at
  3 and 7 and other textures at 0 and 1, every column against the CPU's colour as the check reads it.
- refused_step_ps: a loop an int counts from 1, which a title setting the int by name couldn't count, refused.
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

# The tables' sets and types (XenosRecomp:XenosRecomp/constant_table.h): classes scalar 0, vector 1, matrix rows 2 and
# columns 3, object 4; types bool 1, int 2, float 3, sampler2D 12, samplerCUBE 14.
SCALAR, VECTOR, ROWS, COLUMNS, OBJECT = range(5)
BOOL, INT, FLOAT, SAMPLER_2D, SAMPLER_CUBE = 1, 2, 3, 12, 14


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def compiled(name, optimize):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
    return blob, manifest, native.constant_table(blob)


def entry(table, name, register_set):
    found = [e for e in table if e[0] == name and e[1] == register_set]
    if len(found) != 1:
        fail(f"{name} in the {register_set} set: {found}")
    return found[0]


def literals(manifest):
    return {e[0]: list(e[1]) for e in manifest if isinstance(e[0], int)}


def slots(optimize, tag):
    def fetch(slot, coords):
        return [slot + 0.25] * 4
    for name, want, names in (("hlsl/binding/slots_ps.hlsl", [1.25, 4.25, 0.25, 2.25], "ABCD"),
                              ("hlsl/binding/slots.frag", [1.25, 3.25, 0.25, 2.25], None)):
        blob, manifest, table = compiled(name, optimize)
        got = simulate.run(blob, {0: [0.5, 0.5, 0.0, 0.0]}, literals(manifest), texture_fetch=fetch).exports.get(0)
        if got != want:
            fail(f"{name}, {tag}: samples fetch constants {[x - 0.25 for x in got or []]}, "
                 f"not {[x - 0.25 for x in want]}")
        if names and [entry(table, n, "sampler")[2] for n in names] != [int(x) for x in want]:
            fail(f"{name}, {tag}: the table's samplers {table}")
    print(f"  PASS {tag}: slots_ps's samplers at fetch constants 1, 4, 0 and 2, its table naming them; slots.frag's "
          "packed 1, 3, 0 and 2")


UV = [0.5, -1.25, 2.0, 0.75]
COLOURS = [[0.25, 0.5, -0.125, 1.0], [1.5, -0.5, 0.75, 0.25]]
SCALE = 3.0


def integers(count, flip, tint):
    c = [UV[k] + COLOURS[0][k] * count for k in range(4)]
    if flip:
        c = [2.0 * x for x in reversed(c)]
    if tint:
        c = [x + y for x, y in zip(c, COLOURS[1])]
    return [x * SCALE for x in c]


def integer_cases(optimize, tag):
    blob, manifest, table = compiled("hlsl/binding/integers_ps.hlsl", optimize)
    flip, count = entry(table, "Flip", "bool"), entry(table, "Count", "int4")
    tint, scale = entry(table, "Tint", "float4"), entry(table, "Scale", "float4")
    if flip[2:] != (131, 1, SCALAR, BOOL, 1, 1, 1) or count[2:] != (18, 1, SCALAR, INT, 1, 1, 1):
        fail(f"integers_ps, {tag}: Flip {flip}, Count {count}, not boolean 131 and loop constant 18")
    if tint[2] < 3 or scale[2] < 3 or tint[2] == scale[2] or tint[4:6] != (SCALAR, BOOL) or scale[4:6] != (SCALAR, INT):
        fail(f"integers_ps, {tag}: Tint {tint} and Scale {scale}, not floats of their own past the block's c0 to c2")
    if [e for e in table if e[0] in ("Flip", "Count") and e[1] == "float4"]:
        fail(f"integers_ps, {tag}: Flip or Count read as a float: {table}")
    for flips in (False, True):
        for tints in (0.0, 1.0):
            for passes in (0, 2):
                regs = literals(manifest)
                regs.update({0: COLOURS[0], 1: COLOURS[1], 2: [99.0] * 4, tint[2]: [tints] * 4, scale[2]: [SCALE] * 4})
                got = simulate.run(blob, {0: UV}, regs, loops={18: passes}, bools={131} if flips else set())
                want = integers(passes, flips, tints)
                if got.exports.get(0) != want:
                    fail(f"integers_ps, {tag}, Flip {flips}, Tint {tints}, Count {passes}: {got.exports.get(0)}, "
                         f"not {want}")
    print(f"  PASS {tag}: integers_ps's Flip at boolean 131, Count at loop constant 18, Tint at c{tint[2]} and Scale "
          f"at c{scale[2]}, each value right")


STEP_UNROLLED = [0.25, -0.5, 1.5, 0.125]


def unrolled(optimize, tag):
    blob, manifest, table = compiled("hlsl/binding/unrolled_ps.hlsl", optimize)
    flip = entry(table, "Flip", "bool")
    for flips in (False, True):
        regs = literals(manifest)
        regs[0] = STEP_UNROLLED
        got = simulate.run(blob, {0: UV}, regs, bools={flip[2]} if flips else set()).exports.get(0)
        c = [u + 2.0 * k for u, k in zip(UV, STEP_UNROLLED)]
        want = [2.0 * x for x in reversed(c)] if flips else c
        if got != want:
            fail(f"unrolled_ps, {tag}, Flip {flips}: {got}, not {want}")
    print(f"  PASS {tag}: unrolled_ps's Flip at boolean {flip[2]}, both values right")


TABLE = [("Env", "sampler", 2, 1, OBJECT, SAMPLER_CUBE, 1, 1, 1), ("M", "float4", 0, 4, COLUMNS, FLOAT, 4, 4, 1),
         ("R", "float4", 4, 4, ROWS, FLOAT, 4, 4, 1), ("L", "float4", 8, 1, VECTOR, FLOAT, 1, 3, 1),
         ("S", "float4", 9, 1, SCALAR, FLOAT, 1, 1, 1), ("Arr", "float4", 10, 3, VECTOR, FLOAT, 1, 4, 3)]


def table_case(optimize, tag):
    _, _, table = compiled("hlsl/binding/table_ps.hlsl", optimize)
    got = [e for e in table if not e[0].startswith("_")]
    if got != TABLE:
        fail(f"table_ps, {tag}: {got}, not {TABLE}")
    print(f"  PASS {tag}: table_ps's table, a matrix by columns and one by rows, a float3, a float, an array of 3, "
          "a cube")


# tests/test_hlsl.c's binding cell: Low's reds and High's greens a texel each across a half, the textures at 0 and 1
# that samplers packed from 0 would read, Step, and each half's Count and Bright.
LOW_RED = [48, 112, 176, 240]
HIGH_GREEN = [240, 176, 112, 48]
DECOYS = {0: (8, 248, 248), 1: (248, 8, 248)}
STEP = [0.125, 0.5, 0.0, 0.0]
HALVES = [(2, True), (5, False)]
HALF_WIDTH, TEXELS, TOLERANCE = 40, 4, 1


def unorm(x):
    return min(255, max(0, int(x * 255.0 + 0.5)))


def texture(slot, coords):
    texel = min(TEXELS - 1, max(0, int(coords[0] * TEXELS)))
    rgb = {3: (LOW_RED[texel], 0, 0), 7: (0, HIGH_GREEN[texel], 0)}.get(slot, DECOYS.get(slot, (0, 0, 0)))
    return [x / 255.0 for x in rgb] + [1.0]


def binding_cell(blob, manifest, table):
    """Each column's 8-bit red, green and blue, both halves drawn as tests/test_hlsl.c draws them: Step at its float
    constant, Bright's boolean and Count's loop constant from the table, the textures at 3 and 7 by D3D9's slots."""
    def at(name, register_set):
        found = [e[2] for e in table if e[0] == name and e[1] == register_set]
        return found[0] if found else 0
    regs = literals(manifest)
    regs[at("Step", "float4")] = STEP
    cell = []
    for count, bright in HALVES:
        for x in range(HALF_WIDTH):
            got = simulate.run(blob, {0: [(x + 0.5) / HALF_WIDTH, 0.0, 0.0, 0.0]}, regs, texture_fetch=texture,
                               loops={at("Count", "int4"): count}, bools={at("Bright", "bool")} if bright else set())
            cell.append(tuple(unorm(v) for v in (got.exports.get(0) or [0.0] * 4)[:3]))
    return cell


def binding_reference():
    return [(LOW_RED[x * TEXELS // HALF_WIDTH], HIGH_GREEN[x * TEXELS // HALF_WIDTH],
             unorm(count * STEP[0] + (STEP[1] if bright else 0.0)))
            for count, bright in HALVES for x in range(HALF_WIDTH)]


def wrong(cell):
    return sum(any(abs(a - b) > TOLERANCE for a, b in zip(held, want)) for held, want in zip(cell, binding_reference()))


def grid_binding(optimize, tag):
    blob, manifest, table = compiled("hlsl/grid_binding_ps.hlsl", optimize)
    places = [entry(table, n, s)[2] for n, s in (("Low", "sampler"), ("High", "sampler"))]
    if places != [3, 7]:
        fail(f"grid_binding_ps, {tag}: Low and High at {places}, not 3 and 7")
    n = wrong(binding_cell(blob, manifest, table))
    if n:
        fail(f"grid_binding_ps, {tag}: {n} of {2 * HALF_WIDTH} columns wrong")
    print(f"  PASS {tag}: grid_binding_ps's cell, both halves, as the CPU's")


def refused():
    try:
        native.compile_spirv(module("hlsl/binding/refused_step_ps.hlsl").blob, XSDK_REGISTERS.pixel,
                             XSDK_REGISTERS.vertex, True)
    except native.CompileError as e:
        if e.status != native.REFUSED or "other than 0 by 1" not in str(e):
            fail(f"refused_step_ps: {e.status}, {e}")
    else:
        fail("refused_step_ps compiled")
    print("  PASS refused_step_ps: refused, a loop an HLSL int counts from other than 0 by 1")


def main():
    refused()
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        slots(optimize, tag)
        integer_cases(optimize, tag)
        unrolled(optimize, tag)
        table_case(optimize, tag)
        grid_binding(optimize, tag)
    print("OK")


if __name__ == "__main__":
    main()
