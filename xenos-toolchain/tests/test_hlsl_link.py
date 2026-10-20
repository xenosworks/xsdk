#!/usr/bin/env python3
"""An HLSL pair linked by semantic (xsdk:docs/scope/50-hlsl.md, step 4), as xsdk_shader_link links it on the console
(xsdk:src/shader.c), through libxsc's xsdk_xsc_link and xsdk_xsc_link_export, which the console calls too
(xsc/native.py, link), the optimiser off and on:

- grid_gaps_vs.hlsl and grid_gaps_ps.hlsl, xsdk_test's pair with gaps: each stage packs its interpolators from 0 in its
  semantics' order, the vertex shader's TEXCOORD0, TEXCOORD3, COLOR1 and FOG at 0 to 3 and the pixel shader's TEXCOORD3
  and COLOR1 at 0 and 1, each record marked; linked, TEXCOORD3's and COLOR1's exports go to 0 and 1, TEXCOORD0's and
  FOG's write nothing, no write mask and the scalar destination absolute, at interpolator 0 (xenia:src/xenia/gpu/
  ucode.h:2041-2059), and SQ_PROGRAM_CNTL exports 2; on xdis/simulate.py the pixel shader then reads the vertex
  shader's COLOR1 and TEXCOORD3, and unlinked it doesn't.
- Every export to an interpolator in every HLSL vertex shader of the testdata, the corpus's 50 among them, sits at one
  of its container's export slots, and each slot holds an ALU instruction, so the link reaches every export and
  nothing else.
- A GLSL pair, vert.spv and frag.spv, links by location and is left as it is; an HLSL vertex shader with a GLSL pixel
  shader is refused.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from check_hlsl import separated, vertex_elements  # noqa: E402

TD = ROOT / "xsc" / "testdata"
REGISTERS = 64
# libxenon's usages (libxenon:drivers/xenos/xe.h:159-172).
U_TEXCOORD, U_COLOR, U_FOG = 5, 10, 11
# An interpolator record's mark for a semantic's (xsdk:src/xsc_private.h, XSC_INTERPOLATOR_SEMANTIC).
SEMANTIC = 1 << 31
# An ALU instruction's first word (xenia:src/xenia/gpu/ucode.h:2046-2062).
DEST_MASK, SCALAR_RELATIVE, EXPORT, WRITE_MASKS = 0x3F, 1 << 14, 1 << 15, 0xFF << 16
# grid_gaps_vs.hlsl's inputs, POSITION, COLOR0 and TEXCOORD0, by usage, each exactly representable.
INPUTS = {0: [0.25, -0.5, 0.5, 1.0], U_COLOR: [0.25, 0.75, 0.0, 1.0], U_TEXCOORD: [0.375, 0.0, 0.0, 0.0]}


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def compiled(path, optimize):
    return native.compile_spirv(path.read_bytes(), REGISTERS, REGISTERS, optimize)[0]


def literals(path, optimize):
    manifest = native.compile_spirv(path.read_bytes(), REGISTERS, REGISTERS, optimize)[1]
    return {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}


def semantics(blob):
    """Each record's (register, usage, index), and whether every one is marked."""
    records, _ = native.records(blob)
    return [(r >> 8 & 0xF, r >> 4 & 0xF, r & 0xF) for r in records], all(r & SEMANTIC for r in records)


def exports(blob):
    """Each ALU instruction's slot and first word, and the slots of those that export to an interpolator."""
    c = container.Container(blob)
    alu = {rec.offset: rec.raw[0] for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0] if rec.kind == "alu"}
    return alu, {slot for slot, word in alu.items() if word & EXPORT and word & DEST_MASK < native.INTERPOLATORS}


def export_count(blob):
    return (container.Container(blob).program_control >> native.EXPORT_COUNT_SHIFT & 0xF) + 1


def run_pair(vertex, pixel, constants):
    elements = vertex_elements(vertex)
    data = [INPUTS[usage] for _, usage, _ in elements]
    got = simulate.run(separated(vertex, elements), {0: [0.0] * 4}, constants[0],
                       vertex_fetch=lambda n, fetch, index: data[fetch.fetch_constant_index()]).exports
    registers = {n: got[n] for n in range(export_count(vertex)) if n in got}
    return simulate.run(pixel, registers, constants[1]).exports.get(0)


def gaps(optimize, tag):
    sources = TD / "hlsl" / "grid_gaps_vs.hlsl.spv", TD / "hlsl" / "grid_gaps_ps.hlsl.spv"
    vertex, pixel = (compiled(s, optimize) for s in sources)
    constants = [literals(s, optimize) for s in sources]
    want_vertex = [(0, U_TEXCOORD, 0), (1, U_TEXCOORD, 3), (2, U_COLOR, 1), (3, U_FOG, 0)]
    want_pixel = [(0, U_TEXCOORD, 3), (1, U_COLOR, 1)]
    if semantics(vertex) != (want_vertex, True) or semantics(pixel) != (want_pixel, True):
        fail(f"grid_gaps, {tag}: records {semantics(vertex)} and {semantics(pixel)}, not {want_vertex} and "
             f"{want_pixel}, marked")
    linked = native.link(vertex, pixel)
    before, slots = exports(vertex)
    after, _ = exports(linked)
    # From the vertex shader's interpolator to the pixel shader's, None for one it doesn't read.
    moves = {0: None, 1: 0, 2: 1, 3: None}
    for slot in sorted(slots):
        word, dest = after[slot], moves[before[slot] & DEST_MASK]
        want = (before[slot] & ~(DEST_MASK | SCALAR_RELATIVE | WRITE_MASKS) if dest is None
                else before[slot] & ~DEST_MASK | dest)
        if word != want:
            fail(f"grid_gaps, {tag}: slot {slot}'s export {before[slot]:#010x} linked {word:#010x}, not {want:#010x}")
    unchanged = {s for s in before if s not in slots and before[s] != after[s]}
    if unchanged or export_count(linked) != 2:
        fail(f"grid_gaps, {tag}: slots {sorted(unchanged)} changed, {export_count(linked)} exported, not 2")
    colour, uv = INPUTS[U_COLOR], INPUTS[U_TEXCOORD]
    want = [colour[0], colour[1], uv[0], 1.0]
    got = run_pair(linked, pixel, constants)
    if got != want:
        fail(f"grid_gaps, {tag}: linked, the pixel shader wrote {got}, not {want}")
    unlinked = run_pair(vertex, pixel, constants)
    if unlinked == want:
        fail(f"grid_gaps, {tag}: unlinked, the pixel shader wrote {unlinked} too")
    print(f"  PASS grid_gaps, {tag}: 4 and 2 interpolators packed, {len(slots)} exports linked, 2 exported, the pixel "
          f"shader's COLOR1 and TEXCOORD3 the vertex shader's; unlinked {unlinked}")


def export_slots(optimize, tag):
    count = 0
    for spv in sorted((TD / "hlsl").rglob("*_vs.hlsl.spv")):
        try:
            blob = compiled(spv, optimize)
        except native.CompileError:
            continue
        alu, interpolator_exports = exports(blob)
        slots = {s & 0xFFF for s in native.records(blob)[1]}
        if not interpolator_exports <= slots or not slots <= set(alu):
            fail(f"{spv.name}, {tag}: exports at {sorted(interpolator_exports)}, export slots {sorted(slots)}, ALU "
                 f"instructions at {sorted(alu)}")
        count += 1
    if count < 50:
        fail(f"{tag}: {count} vertex shaders compiled, not the corpus's 50 and more")
    print(f"  PASS {count} HLSL vertex shaders, {tag}: every export to an interpolator at an export slot, each an ALU "
          f"instruction")


def location_pairs():
    vertex, pixel = compiled(TD / "vert.spv", True), compiled(TD / "frag.spv", True)
    if native.link(vertex, pixel) != vertex:
        fail("vert.spv and frag.spv, GLSL's, changed by the link")
    hlsl = compiled(TD / "hlsl" / "grid_gaps_vs.hlsl.spv", True)
    try:
        native.link(hlsl, pixel)
    except native.CompileError as e:
        if e.status != native.REFUSED:
            fail(f"grid_gaps_vs.hlsl with frag.spv: {e.status}, {e}")
        print("  PASS a GLSL pair left as it is, an HLSL vertex shader with a GLSL pixel shader refused")
        return
    fail("grid_gaps_vs.hlsl with frag.spv linked")


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        gaps(optimize, tag)
        export_slots(optimize, tag)
    location_pairs()
    print("OK")


if __name__ == "__main__":
    main()
