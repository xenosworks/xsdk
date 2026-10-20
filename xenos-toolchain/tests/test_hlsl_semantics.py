#!/usr/bin/env python3
"""D3D9's semantics from glslang's HLSL (xsdk:docs/scope/50-hlsl.md, step 4; xsdk:src/xsc_resolve.c), each shader of
xsc/testdata/hlsl on xdis/simulate.py, the optimiser off and on, every export exact:

- semantics_vs.hlsl: each input fetched by the usage and index its semantic names; POSITION and PSIZE, struct members,
  the position and the point's size, with the sprite's export mode; COLOR0, FOG, TEXCOORD1, TEXCOORD0 and COLOR1
  packed from interpolator 0 in their semantics' order, usage then index, at 2, 4, 1, 0 and 3, and 5 exported.
- semantics_ps.hlsl: TEXCOORD1, COLOR1, FOG and COLOR0 packed so too, read from 0, 2, 3 and 1; COLOR1, DEPTH and COLOR0,
  declared in that order, to oC1, the depth and oC0.
- The two linked as xsdk_shader_link links them (xsc/native.py, link): the linked vertex shader's exports, as many as
  it counts, are the pixel shader's registers, as the GPU hands them over, and the pixel shader's results are the
  vertex shader's inputs' by semantic.
- vpos_vface_ps.hlsl: VPOS the pixel parameters' |xy| plus c255's zw, which xsdk keeps at 0 and a band's top row, so
  the pixel's whole column and row; VFACE +1 where the parameters' x is positive or +0 and -1 where it's negative or
  -0, the facing gl_FrontFacing reads (xsdk:docs/HARDWARE.md, "LODs, facing and vertex textures, vulkan4xenon
  milestone 22").
- vpos_wide_ps.hlsl: a float4 VPOS's z and w read 0, whatever the parameters' z and w hold.
- refused_*.hlsl: a vertex input and an interpolator of a semantic that's no D3D9 usage, and two inputs of one
  semantic, each refused with its reason.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from check_hlsl import separated, vertex_elements  # noqa: E402

TD = ROOT / "xsc" / "testdata" / "hlsl"
REGISTERS = 64
# The exports past the interpolators: the depth, the position and the point's size (xenia:src/xenia/gpu/ucode.h:
# 1813-1842).
DEPTH, POSITION, POINT_SIZE = 61, 62, 63
# libxenon's usages (libxenon:drivers/xenos/xe.h:159-172).
U_POSITION, U_BLENDWEIGHT, U_BLENDINDICES, U_NORMAL, U_TEXCOORD, U_TANGENT, U_BINORMAL, U_COLOR = 0, 1, 2, 3, 5, 6, 7, 10
# SQ_PROGRAM_CNTL's vs_export_count, one less than the interpolators, and vs_export_mode, 2 for a point's size
# (xenia:src/xenia/gpu/registers.h:136-144).
EXPORT_COUNT_SHIFT, EXPORT_MODE_SHIFT, SPRITE = 20, 24, 2
FRAG_COORD = native.FRAG_COORD_CONSTANT
OFFSET = [0.5, -0.25, 1.0, 0.0]
# semantics_vs.hlsl's inputs by (usage, index), each exactly representable, so no sum rounds.
INPUTS = {(U_NORMAL, 0): [0.25, 0.5, -0.75, 1.0], (U_POSITION, 0): [1.0, 2.0, 3.0, 1.0],
          (U_BLENDWEIGHT, 0): [0.125, 0.375, 0.5, 0.0], (U_BLENDINDICES, 0): [3.0, 5.0, 7.0, 9.0],
          (U_TANGENT, 0): [-0.5, 0.25, 0.125, 2.0], (U_BINORMAL, 0): [4.0, -2.0, 1.5, 0.5],
          (U_COLOR, 1): [0.25, 0.125, 0.0625, 0.5], (U_TEXCOORD, 3): [0.75, -1.5, 2.25, -3.0]}


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def compiled(name, optimize):
    """The container, and the constants its literals take."""
    blob, manifest, _ = native.compile_spirv((TD / f"{name}.spv").read_bytes(), REGISTERS, REGISTERS, optimize)
    return blob, {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}


def add(a, b, sign=1.0):
    return [x + sign * y for x, y in zip(a, b)]


def scale(a, k):
    return [x * k for x in a]


def run_vertex(blob, literals):
    elements = vertex_elements(blob)
    data = [INPUTS[(usage, index)] for _, usage, index in elements]
    result = simulate.run(separated(blob, elements), {0: [0.0] * 4}, {**literals, 0: OFFSET},
                          vertex_fetch=lambda n, fetch, index: data[fetch.fetch_constant_index()])
    return elements, result.exports


def vertex_semantics(optimize, tag):
    blob, literals = compiled("semantics_vs.hlsl", optimize)
    elements, exports = run_vertex(blob, literals)
    usages = sorted((usage, index) for _, usage, index in elements)
    if usages != sorted(INPUTS):
        fail(f"semantics_vs.hlsl, {tag}: elements {usages}, not {sorted(INPUTS)}")
    i = {k: v for k, v in INPUTS.items()}
    want = {POSITION: add(i[(U_POSITION, 0)], OFFSET), POINT_SIZE: [i[(U_BLENDINDICES, 0)][1] + 1.0],
            2: scale(i[(U_COLOR, 1)], 2.0), 4: [i[(U_BLENDWEIGHT, 0)][0]],
            1: add(i[(U_NORMAL, 0)], i[(U_TANGENT, 0)]), 0: i[(U_TEXCOORD, 3)],
            3: add(i[(U_BINORMAL, 0)], i[(U_BLENDWEIGHT, 0)], -1.0)}
    for reg, values in want.items():
        if exports.get(reg, [None])[:len(values)] != values:
            fail(f"semantics_vs.hlsl, {tag}: export {reg} {exports.get(reg)}, not {values}")
    control = container.Container(blob).program_control
    count, mode = (control >> EXPORT_COUNT_SHIFT & 0xF) + 1, control >> EXPORT_MODE_SHIFT & 7
    if count != 5 or mode != SPRITE:
        fail(f"semantics_vs.hlsl, {tag}: {count} interpolators exported, mode {mode}; not 5 and the sprite's")
    print(f"  PASS semantics_vs.hlsl, {tag}: inputs by usage, POSITION, PSIZE and 5 interpolators by semantic")
    return blob, literals


# TEXCOORD1, COLOR0, COLOR1 and FOG, in the order they pack.
PS_IN = {0: [0.5, 1.5, -2.0, 0.25], 1: [1.0, 0.5, 0.25, 0.75], 2: [0.125, -0.25, 0.5, 1.0], 3: [3.0, 0.0, 0.0, 0.0]}


def pixel_want(r):
    return {0: add(r[1], r[2]), 1: scale(r[0], r[3][0]), DEPTH: [r[1][3] * 0.5]}


def check_pixel(name, tag, exports, want):
    for reg, values in want.items():
        if exports.get(reg, [None])[:len(values)] != values:
            fail(f"{name}, {tag}: export {reg} {exports.get(reg)}, not {values}")


def pixel_semantics(optimize, tag):
    blob, literals = compiled("semantics_ps.hlsl", optimize)
    check_pixel("semantics_ps.hlsl", tag, simulate.run(blob, PS_IN, literals).exports, pixel_want(PS_IN))
    print(f"  PASS semantics_ps.hlsl, {tag}: 4 interpolators by semantic, COLOR1, DEPTH and COLOR0 where they go")
    return blob, literals


def linked(vertex, pixel, tag):
    blob = native.link(vertex[0], pixel[0])
    _, exports = run_vertex(blob, vertex[1])
    count = (container.Container(blob).program_control >> EXPORT_COUNT_SHIFT & 0xF) + 1
    if count != 4:
        fail(f"semantics_vs.hlsl into semantics_ps.hlsl, {tag}: {count} interpolators exported, not the pixel "
             f"shader's 4")
    registers = {n: exports[n] for n in range(count) if n in exports}
    got = simulate.run(pixel[0], registers, pixel[1]).exports
    i = INPUTS
    colour = scale(i[(U_COLOR, 1)], 2.0)
    want = {0: add(colour, add(i[(U_BINORMAL, 0)], i[(U_BLENDWEIGHT, 0)], -1.0)),
            1: scale(add(i[(U_NORMAL, 0)], i[(U_TANGENT, 0)]), i[(U_BLENDWEIGHT, 0)][0]), DEPTH: [colour[3] * 0.5]}
    check_pixel("semantics_vs.hlsl into semantics_ps.hlsl", tag, got, want)
    print(f"  PASS semantics_vs.hlsl into semantics_ps.hlsl, {tag}: linked by semantic")


# (the parameters' x, whether that's a front face): column 3 and column 0 each way.
FACES = ((3.0, True), (-3.0, False), (0.0, True), (-0.0, False))
UV = [0.375, -1.25, 0.0, 0.0]


def vpos_vface(optimize, tag):
    blob, literals = compiled("vpos_vface_ps.hlsl", optimize)
    for top in (0.0, 64.0):
        for x, front in FACES:
            constants = {**literals, FRAG_COORD: [0.5, 0.5 + top, 0.0, top]}
            got = simulate.run(blob, {0: UV, 1: [x, 7.0, 0.0, 0.0]}, constants).exports
            want = [abs(x), 7.0 + top, 1.0 if front else -1.0, UV[0]]
            if got.get(0) != want:
                fail(f"vpos_vface_ps.hlsl, {tag}: x {x} under a band from row {top}: {got.get(0)}, not {want}")
    wide, literals = compiled("vpos_wide_ps.hlsl", optimize)
    got = simulate.run(wide, {0: [5.0, 9.0, 0.25, 0.75]}, {**literals, FRAG_COORD: [0.5, 0.5, 0.0, 0.0]}).exports
    if got.get(0) != [0.0, 0.0, 5.0, 9.0]:
        fail(f"vpos_wide_ps.hlsl, {tag}: {got.get(0)}, not [0, 0, 5, 9]")
    print(f"  PASS vpos_vface_ps.hlsl and vpos_wide_ps.hlsl, {tag}: VPOS whole, in a band too, z and w 0; VFACE +1 "
          f"and -1")


REFUSED = {"refused_input_vs.hlsl": "semantic MYDATA0: not one of D3D9's declaration usages",
           "refused_interpolator_ps.hlsl": "semantic MYDATA0: not one of D3D9's declaration usages",
           "refused_shared_ps.hlsl": "share semantic TEXCOORD0"}
def refusals():
    for name, reason in REFUSED.items():
        try:
            compiled(name, True)
        except native.CompileError as e:
            if e.status != native.REFUSED or reason not in str(e):
                fail(f"{name}: {e.status}, {e}; not refused for {reason!r}")
            continue
        fail(f"{name} compiled")
    print("  PASS an unknown input semantic, an unknown interpolator semantic and a shared semantic refused")


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        vertex = vertex_semantics(optimize, tag)
        pixel = pixel_semantics(optimize, tag)
        linked(vertex, pixel, tag)
        vpos_vface(optimize, tag)
    refusals()
    print("OK")


if __name__ == "__main__":
    main()
