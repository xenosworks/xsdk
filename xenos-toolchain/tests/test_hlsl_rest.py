#!/usr/bin/env python3
"""The rest of what the HLSL corpus and idioms meet (xsdk:docs/scope/50-hlsl.md, step 5). Each shader of
xsc/testdata/hlsl/rest, and xsdk_test's grid_skin_vs.hlsl, runs on xdis/simulate.py, the optimiser off and on, beside
its GLSL twin where one is written, on inputs and constants no operation rounds but the scalar unit's: every export must
be what the HLSL gives exactly, and the twin's bit for bit.

- array_unrolled_ps: static const arrays of floats and float2s at unrolled loops' counters, folded to their elements
  (xsdk:src/xsc_unroll.c, element);
- array_indexed_ps: a float4 array and a float array at a hardware loop's counter, an int in $Global counting it, and at
  an index from TEXCOORD0, read at a0 from literal registers of their own (src/xsc_lower.c, xsdk_xsc_literal_array);
- matrix_cast_vs: a (float3x3) cast of a float4x4 held by rows, its first 3 registers, and a negated vector;
- matrix_tbn_ps: a float3x3 interpolator, a column an interpolator from TEXCOORD1, and the twin's mat3 of three;
  matrix_tbn_vs's float3x3 output linked to it by semantic, xsdk's link at bind (src/shader.c);
- bool_ps: a bool in $Global, a float D3D9 sets 0 or 1, a static const bool4 spirv-opt folds to a shuffle of two
  vectors, and a bool4 of constants and a comparison, the twin's a constant bvec4 select;
- vector_exp_ps: exp, log2, sqrt and sin of vectors, a component each;
- psize_vs: PSIZE an out parameter of its own, the point's size in oPts.x;
- skin_vs: i09's D3DCOLORtoUBYTE4 indices, and grid_skin_vs, int4 BLENDINDICES, picking float4x3 bones, 3 registers
  each, at a0, each bone's rows dot products as wide as the position;
- negate_vs: a scalar int input and an int4's components made floats, one negated, each lane its own.

Refused for what they are: refused_array_ps, a run-time index into an array of values, and refused_rows_vs, a matrix
output held by rows.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from check_hlsl import separated, short, vertex_elements  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402

s = simulate.single
# A vertex shader's position and point size exports (xenia:src/xenia/gpu/ucode.h:1813-1834).
EXPORT_POSITION, EXPORT_POINT_SIZE = 62, 63
# simulate.py's scalar unit: exp2 and log2 of a single, a single.
LOG2_E = s(1.4426950408)


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def same(a, b):
    return len(a) == len(b) and all(x == y and math.copysign(1.0, x) == math.copysign(1.0, y) for x, y in zip(a, b))


def constants(m, manifest, registers):
    """The block's registers from registers, its first, then the literals and c255."""
    blocks = resolve_interface(m).blocks
    regs = {blocks[0].reg + i: list(v) for i, v in enumerate(registers)} if blocks else {}
    regs.update({entry[0]: list(entry[1]) for entry in manifest if isinstance(entry[0], int)})
    regs[native.FRAG_COORD_CONSTANT] = [0.5] * 4
    return regs


def loops(manifest, count):
    return {e[1]: native.loop_passes(e[6], e[7], count) for e in manifest if e[0] == "block_loop"}


def run_pixel(blob, m, manifest, inputs, registers, count=None):
    """Export 0 for interpolators by name, a matrix's a column each."""
    regs = {}
    for v, r, _ in resolve_interface(m).interp_in:
        value = inputs[short(v.name)]
        regs[r] = value.pop(0) if isinstance(value[0], list) else value
    got = simulate.run(blob, regs, constants(m, manifest, registers), loops=loops(manifest, count)).exports
    return got.get(0)


def run_vertex(blob, m, manifest, inputs, registers):
    """Each export by number, for inputs by name."""
    si = resolve_interface(m)
    by_usage = {(usage, index): short(v.name) for v, usage, index in si.vfetch}
    elements = vertex_elements(blob)
    data = [inputs[by_usage[(usage, index)]] for _, usage, index in elements]
    return simulate.run(separated(blob, elements), {0: [0.0] * 4}, constants(m, manifest, registers),
                        vertex_fetch=lambda n, fetch, index: data[fetch.fetch_constant_index()]).exports


def program(name, optimize):
    m = module(name)
    blob, manifest = lower_ex(m, XSDK_REGISTERS, optimize)
    return m, blob, manifest


# ---- the cases' inputs and what the HLSL gives ----

WEIGHTS = [0.5, 0.25, 0.125, 2.0, -1.0]
OFFSETS = [(-1, -1), (1, -1), (-1, 1), (1, 1)]
SCALE = [0.5, 0.0, 0.0, 0.0]


def array_unrolled(uv):
    sum_ = [x * WEIGHTS[0] for x in uv]
    for i in range(1, 5):
        sum_ = [a + b * WEIGHTS[i] * SCALE[0] for a, b in zip(sum_, uv[1:] + uv[:1])]
    o = [0.0, 0.0]
    for j in range(4):
        o = [o[k] + OFFSETS[j][k] * uv[k] * (j + 1) for k in range(2)]
    return [sum_[0] + o[0], sum_[1] + o[1], sum_[2], sum_[3]]


COLOURS = [[0.5, 0.25, 0.125, 1], [-1, 2, 0.75, 0.5], [4, -0.5, 1, 0.25], [0.0625, 8, -2, 1], [3, 1.5, 0.5, -1],
           [-0.25, -4, 6, 2]]
ARRAY_WEIGHTS = [1, 0.5, 2, -1, 0.25, 4]
TINT = [0.25, -0.5, 1.0, 0.125]


def array_indexed(uv, count):
    c = [0.0] * 4
    for i in range(count):
        c = [c[k] + COLOURS[i][k] * ARRAY_WEIGHTS[i] + TINT[k] * uv[0] for k in range(4)]
    j = int(uv[1] * 4.0)
    return [c[k] + COLOURS[j][k] + ARRAY_WEIGHTS[j] for k in range(4)]


WORLD = [[0.5, -0.25, 1.0, 2.0], [0.125, 0.75, -0.5, -1.0], [1.5, 0.0, 0.25, 0.5], [0.0, 0.0, 0.0, 1.0]]
WVP = [[1.0, 0.5, 0.0, -0.25], [0.0, -1.0, 0.25, 0.5], [0.25, 0.0, 2.0, 1.0], [0.0, 0.125, 0.0, 1.0]]
LIGHT = [0.5, -1.0, 0.25, 0.0]
POS = [0.5, -0.25, 1.5, 1.0]
NORMAL = [0.25, 0.5, -0.75, 0.0]


def dot(a, b, n=4):
    return sum(x * y for x, y in zip(a[:n], b[:n]))


def matrix_cast():
    n = [dot(WORLD[i], NORMAL, 3) for i in range(3)]
    neg = [-x for x in LIGHT[:3]]
    return {"position": [dot(WVP[i], POS) for i in range(4)], "n": n, "d": neg + [dot(n, neg, 3)]}


FRAME = [[0.5, 0.25, -1.0, 0.0], [-0.75, 1.0, 0.5, 0.0], [0.125, -0.5, 2.0, 0.0]]
V = [1.5, -0.5, 0.25, 0.0]


def tbn(frame):
    return [sum(V[r] * frame[r][k] for r in range(3)) for k in range(3)] + [1.0]


A = [[0.75, -0.5, 0.25, 2.0], [0.25, 1.5, -1.0, 0.5]]
B = [-0.125, 0.5, 4.0, -2.0]
BOOL_TINT = [0.5, 2.0, -1.0, 0.25]


def bool_case(a, use):
    c = [a[0], B[1], a[2], B[3]]
    t = BOOL_TINT if use else [1.0] * 4
    m = [True, False, a[0] > 0.5, True]
    c3 = [a[0] * 2.0, B[1] + 1.0, a[2] * 2.0]
    return [(c[k] if m[k] else B[k]) * t[k] + c3[0] + 2.0 * c3[1] + 4.0 * c3[2] for k in range(4)]


EXP_IN = [0.5, -0.75, 1.25, 0.25]


def vector_exp():
    a = EXP_IN
    e = [s(2.0 ** s(x * LOG2_E)) for x in a[:2]]
    lg = [s(math.log2(s(x + 2.0))) for x in a[2:]]
    roots = [s(math.sqrt(s(x * x))) for x in (a[1], a[3])]
    sines = [s(math.sin(x)) for x in (a[0], a[2])]
    return [s(x + y) for x, y in zip(e + lg, roots + sines)]


SIZE = [24.0, 0.0, 0.0, 0.0]
PSIZE_POS = [0.25, -0.5, 0.5, 2.0]

# 8 float4x3 bones, 3 registers each, a register a row of glslang's matrix: output component j is row j . position.
BONES = [[[(b + 1) * 0.125 * (j + 1), 0.25 * (j - 1), 0.5 if j == 2 else 0.0, b * 0.0625 - j * 0.25]
          for j in range(3)] for b in range(8)]
SKIN_POS = [0.5, -1.5, 0.25, 1.0]
SKIN_WEIGHTS = [0.5, 0.25, 0.125, 0.0]
SKIN_INDICES = [5, 2, 7, 0]
SKIN_UV = [0.375, 0.625, 0.0, 0.0]


def skinned():
    w = SKIN_WEIGHTS[:3] + [1.0 - sum(SKIN_WEIGHTS[:3])]
    return [sum(w[k] * dot(BONES[SKIN_INDICES[k]][j], SKIN_POS) for k in range(4)) for j in range(3)]


NEGATE_POS = [0.75, -2.0, 0.5, 1.0]
# Each lane its own, so a scalar read from the wrong one shows.
NEGATE_K = [3.0, 7.0, 11.0, 13.0]
NEGATE_IDX = [5.0, 2.0, 9.0, 4.0]


def ubyte4(indices):
    """A D3DCOLOR's bytes as UBYTE4N fetches them, z, y, x and w the indices D3DCOLORtoUBYTE4 gives."""
    x, y, z, w = indices
    return [s(z / 255.0), s(y / 255.0), s(x / 255.0), s(w / 255.0)]


def bone_registers():
    return [row for bone in BONES for row in bone]


# ---- the checks ----

def twin_pair(hlsl, glsl, optimize, run):
    """Each program's result through run, the HLSL's first, the GLSL twin's or None."""
    got = run(*program(hlsl, optimize))
    return got, run(*program(glsl, optimize)) if glsl else None


def expect(label, got, twin, want):
    want = [s(x) for x in want]
    if got is None or not same(got[:len(want)], want):
        fail(f"{label}: {got}, not {want}")
    if twin is not None and not same(twin[:len(want)], got[:len(want)]):
        fail(f"{label}: the GLSL twin {twin}, the HLSL {got}")


def pixel_cases(optimize, tag):
    for uv in ([0.375, -1.25, 0.5, 2.0], [-0.5, 0.25, -1.0, 0.75]):
        got, twin = twin_pair("hlsl/rest/array_unrolled_ps.hlsl", "hlsl/rest/array_unrolled.frag", optimize,
                              lambda m, b, f: run_pixel(b, m, f, {"uv": list(uv)}, [SCALE]))
        expect(f"array_unrolled_ps, {tag}, uv {uv}", got, twin, array_unrolled(uv))
    for count in (0, 3, 6):
        for uv in ([0.5, 0.0], [-0.25, 0.375], [0.75, 0.875], [1.0, 1.375]):
            got, twin = twin_pair("hlsl/rest/array_indexed_ps.hlsl", "hlsl/rest/array_indexed.frag", optimize,
                                  lambda m, b, f: run_pixel(b, m, f, {"uv": uv + [0.0, 0.0]},
                                                            [TINT, [float(count), 0, 0, 0]], count))
            expect(f"array_indexed_ps, {tag}, count {count}, uv {uv}", got, twin, array_indexed(uv, count))
    got, twin = twin_pair("hlsl/rest/matrix_tbn_ps.hlsl", "hlsl/rest/matrix_tbn.frag", optimize,
                          lambda m, b, f: run_pixel(b, m, f, {"v": V, "tbn": [list(r) for r in FRAME], "t": FRAME[0],
                                                              "b": FRAME[1], "n": FRAME[2]}, []))
    expect(f"matrix_tbn_ps, {tag}", got, twin, tbn(FRAME))
    for a in A:
        for use in (0.0, 1.0):
            got, twin = twin_pair("hlsl/rest/bool_ps.hlsl", "hlsl/rest/bool.frag", optimize,
                                  lambda m, b, f: run_pixel(b, m, f, {"a": a, "b": B}, [[use, 0, 0, 0], BOOL_TINT]))
            expect(f"bool_ps, {tag}, a {a}, UseTint {use}", got, twin, bool_case(a, use))
    got, twin = twin_pair("hlsl/rest/vector_exp_ps.hlsl", "hlsl/rest/vector_exp.frag", optimize,
                          lambda m, b, f: run_pixel(b, m, f, {"a": EXP_IN}, []))
    expect(f"vector_exp_ps, {tag}", got, twin, vector_exp())
    print(f"  PASS {tag}: array_unrolled_ps, array_indexed_ps at counts 0, 3 and 6, matrix_tbn_ps, bool_ps and "
          f"vector_exp_ps exact, as their twins")


def exports_by_name(m, exports):
    out = {short(v.name): exports.get(o) for v, o, _ in resolve_interface(m).interp_out}
    out["position"], out["size"] = exports.get(EXPORT_POSITION), exports.get(EXPORT_POINT_SIZE)
    return out


def vertex_run(inputs, registers):
    return lambda m, b, f: exports_by_name(m, run_vertex(b, m, f, inputs, registers))


def vertex_cases(optimize, tag):
    got, twin = twin_pair("hlsl/rest/matrix_cast_vs.hlsl", "hlsl/rest/matrix_cast.vert", optimize,
                          vertex_run({"pos": POS, "n": NORMAL}, WORLD + WVP + [LIGHT]))
    want = matrix_cast()
    for name, twin_name in (("position", "position"), ("n", "on"), ("d", "d")):
        expect(f"matrix_cast_vs {name}, {tag}", got[name], twin[twin_name], want[name])
    got, twin = twin_pair("hlsl/rest/psize_vs.hlsl", "hlsl/rest/psize.vert", optimize,
                          vertex_run({"pos": PSIZE_POS}, [SIZE]))
    expect(f"psize_vs's size, {tag}", got["size"], twin["size"], [SIZE[0] * PSIZE_POS[3]])
    expect(f"psize_vs's position, {tag}", got["position"], twin["position"], PSIZE_POS)
    inputs = {"pos": SKIN_POS, "weights": SKIN_WEIGHTS, "indices": ubyte4(SKIN_INDICES)}
    got, twin = twin_pair("hlsl/rest/skin_vs.hlsl", "hlsl/rest/skin.vert", optimize,
                          vertex_run(inputs, bone_registers()))
    expect(f"skin_vs, {tag}", got["position"], twin["position"], skinned() + [1.0])
    inputs = {"pos": SKIN_POS, "weights": SKIN_WEIGHTS, "indices": [float(i) for i in SKIN_INDICES], "uv": SKIN_UV}
    got, twin = twin_pair("hlsl/grid_skin_vs.hlsl", "hlsl/rest/grid_skin.vert", optimize,
                          vertex_run(inputs, bone_registers()))
    p = skinned()
    expect(f"grid_skin_vs's position, {tag}", got["position"], twin["position"], p[:2] + [0.5, 1.0])
    expect(f"grid_skin_vs's COLOR0, {tag}", got["colour"], twin["colour"], [p[2], 0.75, 0.0, 1.0])
    expect(f"grid_skin_vs's TEXCOORD0, {tag}", got["uv"], twin["ouv"], SKIN_UV[:2])
    m, blob, manifest = program("hlsl/rest/negate_vs.hlsl", optimize)
    got = run_vertex(blob, m, manifest, {"pos": NEGATE_POS, "k": NEGATE_K, "idx": NEGATE_IDX}, [])
    k, idx = NEGATE_K[0], NEGATE_IDX
    expect(f"negate_vs, {tag}", got.get(EXPORT_POSITION), None, [NEGATE_POS[0], -k, idx[2], idx[1] + k])
    print(f"  PASS {tag}: matrix_cast_vs, psize_vs, skin_vs and grid_skin_vs exact, as their twins, and negate_vs")


def linked_frame(optimize, tag):
    """matrix_tbn_vs's frame, its third row doubled, through the link into matrix_tbn_ps."""
    vm, vertex, vf = program("hlsl/rest/matrix_tbn_vs.hlsl", optimize)
    pm, pixel, pf = program("hlsl/rest/matrix_tbn_ps.hlsl", optimize)
    records, _ = native.records(vertex)
    if [(r >> 4 & 0xF, r & 0xF) for r in records] != [(5, 0), (5, 1), (5, 2), (5, 3)]:
        fail(f"matrix_tbn_vs, {tag}: records {[hex(r) for r in records]}, not TEXCOORD0 to 3")
    linked = native.link(vertex, pixel)
    inputs = {"pos": POS, "t": FRAME[0], "b": FRAME[1], "n": FRAME[2], "v": V}
    exports = run_vertex(linked, vm, vf, inputs, [])
    got = simulate.run(pixel, {n: exports[n] for n in range(4)}, constants(pm, pf, [])).exports.get(0)
    expect(f"matrix_tbn_vs linked to matrix_tbn_ps, {tag}", got, None, tbn(FRAME[:2] + [[2 * x for x in FRAME[2]]]))
    print(f"  PASS {tag}: matrix_tbn_vs's float3x3 TEXCOORD1 to 3, linked, is matrix_tbn_ps's")


REFUSALS = {
    "refused_array_ps.hlsl": "a run-time index into a local array other than a constant one",
    "refused_rows_vs.hlsl": "a matrix held by rows",
}


def refused():
    for name, why in REFUSALS.items():
        try:
            native.compile_spirv(module(f"hlsl/rest/{name}").blob, XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, True)
        except native.CompileError as e:
            if e.status != native.REFUSED or why not in str(e):
                fail(f"{name}: {e.status}, {e}")
        else:
            fail(f"{name} compiled")
        print(f"  PASS {name}: refused, {why}")


def main():
    refused()
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        pixel_cases(optimize, tag)
        vertex_cases(optimize, tag)
        linked_frame(optimize, tag)
    print("OK")


if __name__ == "__main__":
    main()
