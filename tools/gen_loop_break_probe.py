#!/usr/bin/env python3
"""gen_loop_break_probe.py <out.h>: the shaders for probe_loop_break, milestone 46's console probe of hardware loops
(docs/scope/46-loops.md, step 1): LOOP_END's predicated break where the pixels of one vector disagree, the design's
gated values, early exit, nested loops, a0 around a loop, a sample in a pass, and the same in vertex shaders.

Each shader is built by hand with xost's encoders. A pixel shader's interpolator holds u and v across its 10-pixel quad,
the quad's base plus a half, and 1.5; floored, they give px, py, base and 1, and a pixel's break pass is
B = K . (px, py, base, 1) + J where px + py is odd, K and J rows each draw sets. A vertex shader reads (Bq, 0, 0, 1)
for its quad and takes B = K . that. Nothing in a pass is predicated: values are gated by cnd selects, as the scope's
design has it, and only LOOP_END breaks on the predicate.

The generator stops unless xost's simulator (xenos-toolchain/xdis/simulate.py, one pixel at a time, breaking a loop on
that pixel's own p0 as Xenia's translator does) gives each draw the bytes the probe's check expects at a spread of
pixels; unless each break build fails a check on the simulator; and unless the loop control flow builders write the
words xost writes for vk_loop_count_if.frag's hardware loop, whose loops have run on the console (docs/HARDWARE.md,
"Branches, vulkan4xenon milestone 21").
"""
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import gen_triangle_shaders as tri  # noqa: E402
import gen_xsc_probe as pack  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from gen_triangle_shaders import ce, enc, xu  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
CF = xu.ControlFlowOpcode
CHECK = 0x5A
EXEC_MOST = 6
COLUMNS, ROWS, CELL = 128, 72, 10
# Pixel shaders' registers: the interpolator, its floor, the parity, B, then each shape's own.
INTERP, FLOORED, PARITY, BREAK, STATE, STOPPED, FLAGS, NEXT, COND, OUT = range(10)
# A vertex shader fetches its position and its quad's data where a pixel shader floors and takes the parity.
POSITION, DATA = 1, 2
# The a0 shapes' result and the sample shapes' registers.
RESULT = 2
LOD, SUM, TEXEL, COORD, DIGIT = 4, 5, 6, 7, 8
REGISTERS = OUT + 1
# Pixel shaders take loop constants from 16, vertex shaders from 0, as xost splits them (src/xsc_lower_flow.c:9-16).
PIXEL_FIRST_LOOP, VERTEX_FIRST_LOOP = 16, 0
# c[n] below FILL_ROWS holds n, 255 - n and n ^ CHECK for the a0 shapes, which read c[A0_ROW + base].
FILL_ROWS = 200
A0_ROW = 64
ROW_K, ROW_J = 200, 201
# The gated w: B + 49 where B is the pass a pixel leaves at.
GATED_W = 49
# The inner loop's break in the nested shape with one, after this many passes of each outer pass.
INNER_PASSES = 2
TEXTURE_SIZE, TEXTURE_LEVELS, TEXTURE_SLOT = 128, 8, 1
LITERALS = [
    ("half", (0.5, 0.5, 0.0, 0.0)), ("one", (1.0,) * 4), ("zero", (0.0,) * 4), ("scale", (1 / 255,) * 4),
    ("bias", (0.25 / 255,) * 4), ("count_scale", (1 / 255, 1 / 255, 0.0, 0.0)),
    ("check", (0.25 / 255, 0.25 / 255, (CHECK + 0.25) / 255, 1.0)), ("step", (1.0, -1.0, 0.0, 0.0)),
    ("x", (1.0, 0.0, 0.0, 0.0)), ("y", (0.0, 1.0, 0.0, 0.0)), ("z", (0.0, 0.0, 1.0, 0.0)), ("w", (0.0, 0.0, 0.0, 1.0)),
    ("gated_step", (1.0, 0.0, 1.0, 0.0)), ("fifty", (GATED_W + 1.0,) * 4),
    ("gated_init", (0.0, GATED_W + 1.0, 0.0, 0.0)),
    ("xyw", (1.0, 1.0, 0.0, 1.0)), ("outer_step", (1.0, 0.0, float(INNER_PASSES), 0.0)),
    ("inner_step", (0.0, 1.0, -1.0, 0.0)), ("uv_scale", (1 / TEXTURE_SIZE,) * 4), ("xzw", (1.0, 0.0, 1.0, 1.0)),
    ("quarter", (0.25,) * 4), ("four", (4.0,) * 4), ("minus_half", (-0.5,) * 4),
    ("digit_scale", (255 / 32,) * 4), ("digit_out", (0.0, 1 / 255, 0.0, 0.0)),
    ("digit_out_bias", (0.0, 0.25 / 255, 0.0, 0.0)),
]
LITERAL_FIRST = 202
ROW = {name: LITERAL_FIRST + i for i, (name, _) in enumerate(LITERALS)}
# Below the group from c252 that xsdk keeps (include/xsdk/gpu.h, XSDK_PIXEL_CONSTANTS).
assert LITERAL_FIRST + len(LITERALS) <= 252 and A0_ROW + 101 < FILL_ROWS
# xost's template fetch: the arbitrary filter's zero value and the aniso filter from the fetch constant
# (src/xsc_encode.c, xsdk_xsc_template_tfetch).
DEST_KEEP = 0o7777
SRC_XY = 0 | 1 << 2
# The levels setTexLOD picks among in the shapes without a loop, by the quad's base or the pixel's column.
SET_LEVELS = 4
BREAKS = {
    "nogate": "gating removed: each pass's end copies its values, so a pixel that left takes later passes'",
    "invert": "the predicated break's condition inverted",
    "swap": "the outer and inner loops' constants swapped",
    "hoist": "the sample taken once before the loop",
    "noa0": "a0 never loaded in 5a's pass",
}


@dataclass
class Op:
    words: tuple
    fetch: bool = False
    serialize: bool = False
    setp: bool = False
    vertex_fetch: bool = False


def swz(*components):
    """A component-relative swizzle (xenia:src/xenia/gpu/ucode.h:1977-1982)."""
    return sum(((c - k) % 4) << (2 * k) for k, c in enumerate(components))


def splat(component):
    return swz(component, component, component, component)


def temp(reg, swizzle=0, negate=False):
    return reg, True, swizzle, negate


def const(row):
    return (ROW[row] if isinstance(row, str) else row), False, 0, False


def alu(opcode, dest, mask, *sources, export=False, serialize=False):
    fields = dict(vector_opcode=opcode, vector_dest=dest, vector_write_mask=mask, is_export=export)
    for n, (reg, is_temp, swizzle, negate) in enumerate(sources, 1):
        fields.update({f"src{n}_reg": reg, f"src{n}_is_temp": is_temp, f"src{n}_swizzle": swizzle,
                       f"src{n}_negate": negate})
    return Op(enc.build_alu(**fields), serialize=serialize)


def scalar(opcode, reg, component, negate=False):
    """A scalar op on one component of a temp, as xost tests a condition (src/xsc_lower_flow.c, emit_setp)."""
    words = enc.build_alu(scalar_opcode=opcode, src3_reg=reg, src3_swizzle=splat(component), src3_negate=negate)
    return Op(words, setp=opcode in (S.kSetpEq, S.kSetpGe))


def read(row, dest):
    """xost's c[row+a0] read, max of it with itself (src/xsc_lower_block.c, xsdk_xsc_block_register)."""
    return Op(enc.build_alu(vector_dest=dest, vector_write_mask=0xF, is_const_0_addressed=True,
                            is_const_1_addressed=True, is_const_address_register_relative=True, src1_reg=row,
                            src1_is_temp=False, src2_reg=row, src2_is_temp=False))


def export(source, scale, bias):
    """oC0 or o0, both export 0, as source * scale + bias."""
    return alu(V.kMad, tri.EXPORT_COLOR_0, 0xF, temp(source), const(scale), const(bias), export=True)


def tfetch(source, computed, serialize=True, gradients=False):
    """A 2D sample from TEXTURE_SLOT as xost's template has it, a quarter level on so a whole LOD picks its level
    whether the GPU rounds or floors; at the LOD setTexLOD gave, or the one the GPU works out, from the coordinates or
    the gradients' setters."""
    return Op(enc.build_tfetch(src=source, src_swizzle=SRC_XY, dest=TEXEL, dest_swizzle=tri.DEST_XYZW,
                               fetch_constant_index=TEXTURE_SLOT, fetch_valid_only=True, arbitrary_filter=0,
                               use_computed_lod=computed, use_register_lod=not computed,
                               use_register_gradients=gradients, sample_location=xu.SampleLocation.kCentroid,
                               lod_bias=0.25),
              fetch=True, serialize=serialize)


def set_lod(source, component):
    """setTexLOD from one component, as xost writes it (src/xsc_lower_sample.c, set_state)."""
    return Op(enc.build_tfetch(opcode=xu.FetchOpcode.kSetTextureLod, src=source, src_swizzle=component,
                               dest_swizzle=DEST_KEEP, fetch_valid_only=True, arbitrary_filter=0,
                               sample_location=xu.SampleLocation.kCentroid), fetch=True, serialize=True)


def control_flow(items):
    """The control flow and instructions for items: ("exec", ops), ("alloc", type), ("loop", id) and
    ("end", id, condition), condition None for no break; and the first instruction's slot."""
    entries = []
    for item in items:
        if item[0] == "exec":
            entries += [("exec", item[1][i:i + EXEC_MOST]) for i in range(0, len(item[1]), EXEC_MOST)]
        else:
            entries.append(item)
    last = max(n for n, e in enumerate(entries) if e[0] == "exec")
    first = address = (len(entries) + 1) // 2
    flow, instructions, open_loops = [None] * len(entries), [], []
    for n, entry in enumerate(entries):
        if entry[0] == "exec":
            ops = entry[1]
            # Two sequence bits an instruction, a fetch and a serialize (ucode.h:230-232).
            sequence = sum((op.fetch | op.serialize << 1) << 2 * k for k, op in enumerate(ops))
            vertex_cache = (1 << sum(op.vertex_fetch for op in ops)) - 1
            flow[n] = enc.build_cf_exec(CF.kExecEnd if n == last else CF.kExec, address=address, count=len(ops),
                                        sequence=sequence, vc_hi=vertex_cache & 0xF, vc_lo=vertex_cache >> 4,
                                        is_predicate_clean=not any(op.setp for op in ops))
            instructions += [op.words for op in ops]
            address += len(ops)
        elif entry[0] == "alloc":
            flow[n] = enc.build_cf_alloc(alloc_type=entry[1])
        elif entry[0] == "loop":
            open_loops.append(n)
        else:
            start = open_loops.pop()
            flow[start] = enc.build_cf_loop_start(address=n + 1, loop_id=entry[1])
            flow[n] = enc.build_cf_loop_end(address=start + 1, loop_id=entry[1],
                                            is_predicated_break=entry[2] is not None, condition=bool(entry[2]))
    assert not open_loops
    return tri.pack(flow, instructions), first


def pixel_shader(body, post):
    dwords, _ = control_flow([("alloc", tri.ALLOC_COLORS), *body, ("exec", post)])
    return ce.build_container(
        "pixel", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(ps_num_reg=REGISTERS - 1, ps_export_mode=tri.PS_EXPORT_MODE_COLOR),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)


def vertex_shader(body, post):
    """The fetches, the loop, then the position's and the interpolator's exports, each after its alloc."""
    fetches = [Op(tri.fetch(reg), fetch=True, vertex_fetch=True) for reg in (POSITION, DATA)]
    dwords, first = control_flow([("exec", fetches), *body, ("alloc", tri.ALLOC_POSITION),
                                  ("exec", [Op(tri.export(tri.EXPORT_POSITION, POSITION))]),
                                  ("alloc", tri.ALLOC_INTERPOLATORS), ("exec", post)])
    return ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=REGISTERS - 1, vs_resource=1, vs_export_count=0),
        context_misc=0, interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        vertex_elements=[ce.VertexElement(first, tri.USAGE_POSITION, 0),
                         ce.VertexElement(first + 1, ce.USAGE_COLOR, 0)],
        creator=tri.CREATOR)


def break_pass(vertex):
    """B in all four of BREAK: K . (px, py, base, 1) + J * 2 * frc((px + py) / 2), or K . the quad's data."""
    if vertex:
        return [alu(V.kDp4, BREAK, 0xF, temp(DATA), const(ROW_K), serialize=True)]
    return [alu(V.kFloor, FLOORED, 0xF, temp(INTERP)),
            alu(V.kDp4, PARITY, 0xF, temp(FLOORED), const("half")),
            alu(V.kFrc, PARITY, 0xF, temp(PARITY)),
            alu(V.kDp4, BREAK, 0xF, temp(FLOORED), const(ROW_K)),
            alu(V.kMad, BREAK, 0xF, temp(PARITY), const(ROW_J), temp(BREAK))]


def loop_id(vertex, level):
    return (VERTEX_FIRST_LOOP if vertex else PIXEL_FIRST_LOOP) + level


def counter(vertex, brk):
    """Case 1's pass counter, never gated, and a count down from B; LOOP_END breaks on p0, set once the count down
    reaches 0 (setp_ge of its negation, ucode.h:1204)."""
    pre = break_pass(vertex) + [alu(V.kMul, STATE, 0xF, temp(BREAK), const("y"))]
    one_pass = [alu(V.kAdd, STATE, 0xF, temp(STATE), const("step")), scalar(S.kSetpGe, STATE, 1, negate=True)]
    post = [alu(V.kMul, OUT, 0xF, temp(BREAK), const("y")),
            alu(V.kMad, OUT, 0xF, temp(STATE, splat(0)), const("x"), temp(OUT)), export(OUT, "count_scale", "check")]
    level = loop_id(vertex, 0)
    return [("exec", pre), ("loop", level), ("exec", one_pass), ("end", level, brk != "invert")], post


def gated(vertex, brk, breaks):
    """The scope's design: i and w gated by running and alive, 1.0 or 0.0 (cnd, ucode.h:1465-1481), a break on i >= B
    between them, and an ungated pass count n; LOOP_END breaks once alive is 0 (setp_eq, ucode.h:1162-1171)."""
    pre = break_pass(vertex) + [alu(V.kMax, STATE, 0xF, const("gated_init"), const("gated_init")),
                                alu(V.kMax, STOPPED, 0xF, temp(STATE), temp(STATE)),
                                alu(V.kMax, FLAGS, 0xF, const("one"), const("one"))]
    alive = temp(FLAGS, splat(1))
    end_of_pass = (alu(V.kMax, STATE, 0xF, temp(NEXT), temp(NEXT)) if brk == "nogate" else
                   alu(V.kCndGt, STATE, 0xF, temp(FLAGS, swz(0, 0, 2, 0)), temp(NEXT), temp(STOPPED)))
    one_pass = [alu(V.kMax, FLAGS, 0x1, alive, alive),
                alu(V.kAdd, NEXT, 0xF, temp(STATE), const("gated_step")),
                alu(V.kSge, COND, 0xF, temp(NEXT, splat(0)), temp(BREAK)),
                alu(V.kMin, COND, 0xF, temp(COND), temp(FLAGS, splat(0))),
                alu(V.kCndGt, STOPPED, 0xF, temp(COND), temp(NEXT), temp(STOPPED)),
                alu(V.kAdd, FLAGS, 0x3, temp(FLAGS), temp(COND, negate=True)),
                alu(V.kAdd, NEXT, 0x2, temp(NEXT, splat(0)), const("fifty")),
                end_of_pass,
                scalar(S.kSetpEq, FLAGS, 1)]
    post = [alu(V.kMul, OUT, 0xF, temp(BREAK), const("z")),
            alu(V.kMad, OUT, 0xF, temp(STATE, swz(0, 1, 2, 2)), const("xyw"), temp(OUT)), export(OUT, "scale", "bias")]
    level = loop_id(vertex, 0)
    condition = (brk != "invert") if breaks else None
    return [("exec", pre), ("loop", level), ("exec", one_pass), ("end", level, condition)], post


def nested(vertex, brk, depth, inner_break):
    """depth loops, each level's pass adding 1 to its own counter; or two, the inner one leaving after INNER_PASSES
    passes of each outer pass on a count down, its LOOP_END breaking on p0."""
    ids = [loop_id(vertex, level) for level in range(depth)]
    if brk == "swap":
        ids[0], ids[1] = ids[1], ids[0]
    if inner_break:
        passes = [[alu(V.kMad, STATE, 0xF, temp(STATE), const("xyw"), const("outer_step"))],
                  [alu(V.kAdd, STATE, 0xF, temp(STATE), const("inner_step")), scalar(S.kSetpGe, STATE, 2, negate=True)]]
    else:
        passes = [[alu(V.kAdd, STATE, 0xF, temp(STATE), const("xyzw"[level]))] for level in range(depth)]
    body = [("exec", [alu(V.kMax, STATE, 0xF, const("zero"), const("zero"))])]
    for level in range(depth):
        body += [("loop", ids[level]), ("exec", passes[level])]
    for level in reversed(range(depth)):
        body.append(("end", ids[level], True if inner_break and level == depth - 1 else None))
    return body, [export(STATE, "scale", "bias")]


def a0_shape(shape, brk):
    """a0 loaded from the interpolator's base (maxasf, as xost loads it, src/xsc_lower_block.c, set_a0) and c[A0_ROW+a0]
    read into RESULT, which each pass first sets to the marker 0: both in the pass, the load before LOOP_START, or the
    load in the pass and the read after LOOP_END. The only alloc, the colours', comes before all of it."""
    marker = alu(V.kAdd, RESULT, 0xF, temp(INTERP), temp(INTERP, negate=True))
    load = [] if brk == "noa0" and shape == "in" else [scalar(S.kMaxAsf, INTERP, 2)]
    pre, one_pass, post = {"in": ([marker], [marker, *load, read(A0_ROW, RESULT)], []),
                           "before": ([marker, *load], [marker, read(A0_ROW, RESULT)], []),
                           "after": ([marker], load, [read(A0_ROW, RESULT)])}[shape]
    level = loop_id(False, 0)
    post = post + [alu(V.kMax, tri.EXPORT_COLOR_0, 0xF, temp(RESULT), temp(RESULT), export=True)]
    return [("exec", pre), ("loop", level), ("exec", one_pass), ("end", level, None)], post


def sample(computed, digits, fetch_waits, brk):
    """A sample each pass, its texel added up and the last kept: at the LOD the pass gives, its index through
    setTexLOD, or at the LOD the GPU works out from coordinates the pass scales by 2 to the index. With digits, each
    pass's level goes into a base-4 digit instead, so the export shows which level each pass took; the fetch after
    setTexLOD serialized, or not, as xost leaves it. The fetches and
    the work reading them are serialized (docs/HARDWARE.md, "Slow fetches across execs")."""
    start = "uv_scale" if computed else "zero"
    pre = [alu(V.kMax, LOD, 0xF, const(start), const(start)), alu(V.kMax, SUM, 0xF, const("zero"), const("zero")),
           alu(V.kMul, COORD, 0xF, temp(INTERP), const("uv_scale"))]
    fetches = [tfetch(COORD, True)] if computed else [set_lod(LOD, 0), tfetch(COORD, False, fetch_waits)]
    if brk == "hoist":
        pre, fetches = pre + fetches, []
    scale = [alu(V.kMul, COORD, 0xF, temp(INTERP), temp(LOD))] if computed and brk != "hoist" else []
    advance = alu(V.kAdd, LOD, 0xF, temp(LOD), temp(LOD) if computed else const("one"))
    # A level's red is 16 + 32L (texel), so red * 255 / 32 - 0.5 is L.
    record = ([alu(V.kMad, DIGIT, 0xF, temp(TEXEL, splat(0)), const("digit_scale"), const("minus_half"),
                   serialize=True), alu(V.kMad, SUM, 0xF, temp(SUM), const("four"), temp(DIGIT))] if digits else
              [alu(V.kAdd, SUM, 0xF, temp(SUM), temp(TEXEL), serialize=True)])
    one_pass = scale + fetches + record + [advance]
    post = ([alu(V.kMad, OUT, 0xF, temp(SUM), const("digit_out"), const("digit_out_bias"))] if digits else
            [alu(V.kMul, OUT, 0xF, temp(SUM), const("y"))])
    post.append(alu(V.kMad, tri.EXPORT_COLOR_0, 0xF, temp(TEXEL), const("xzw"), temp(OUT), export=True))
    level = loop_id(False, 0)
    return [("exec", pre), ("loop", level), ("exec", one_pass), ("end", level, None)], post


def set_gradient(opcode, source, serialize=True):
    """setGradientH or setGradientV from a temp's x and y, as xost writes them (src/xsc_lower_sample.c,
    set_gradients)."""
    return Op(enc.build_tfetch(opcode=opcode, src=source, src_swizzle=SRC_XY, dest_swizzle=DEST_KEEP,
                               fetch_valid_only=True, arbitrary_filter=0, sample_location=xu.SampleLocation.kCentroid),
              fetch=True, serialize=serialize)


def level_sample(component, shape):
    """One sample, no loop, at the level from the quad's base (component 2) or the pixel's column (component 0), mod
    SET_LEVELS: the fraction of (n + 0.5) / 4, times 4, less a half, is n mod 4, the fraction never near its wrap
    however the interpolator rounds. So the pixel groups in flight, or the pixels of one, ask for different levels.
    The shapes: setTexLOD and the fetch both serialized; the fetch not, as xost leaves it; the two alone in an exec
    after the one working the LOD out; or the gradients' setters at 2 to the level over the texture's size, both
    serialized or, as xost leaves them, the first alone."""
    work = [alu(V.kMul, LOD, 0xF, temp(INTERP, splat(component)), const("quarter")),
            alu(V.kFrc, LOD, 0xF, temp(LOD)), alu(V.kMad, LOD, 0xF, temp(LOD), const("four"), const("minus_half")),
            alu(V.kMul, COORD, 0xF, temp(INTERP), const("uv_scale"))]
    export = alu(V.kMax, tri.EXPORT_COLOR_0, 0xF, temp(TEXEL), temp(TEXEL), export=True, serialize=True)
    if shape.startswith("gradients"):
        exp = Op(enc.build_alu(scalar_opcode=S.kExp, scalar_dest=DIGIT, scalar_write_mask=0xF, src3_reg=LOD,
                               src3_swizzle=splat(0)))
        work += [exp, alu(V.kMul, DIGIT, 0xF, temp(DIGIT), const("uv_scale")),
                 alu(V.kMul, SUM, 0xF, temp(DIGIT), const("x")), alu(V.kMul, DIGIT, 0xF, temp(DIGIT), const("y"))]
        fetches = [set_gradient(xu.FetchOpcode.kSetTextureGradientsHorz, SUM),
                   set_gradient(xu.FetchOpcode.kSetTextureGradientsVert, DIGIT, shape == "gradients"),
                   tfetch(COORD, True, False, True)]
    else:
        fetches = [set_lod(LOD, 0), tfetch(COORD, False, shape == "both")]
    if shape == "alone":
        return [("exec", work)], fetches + [export]
    return [], work + fetches + [export]


def shader(key, brk):
    kind, vertex, *args = key
    if kind == "level":
        body, post = level_sample(*args)
    elif kind in ("a0", "sample"):
        body, post = (a0_shape if kind == "a0" else sample)(*args, brk)
    else:
        body, post = {"counter": counter, "gated": gated, "nested": nested}[kind](vertex, brk, *args)
    blob = (vertex_shader if vertex else pixel_shader)(body, post)
    return list(struct.unpack(f">{len(blob) // 4}I", blob))


@dataclass
class Draw:
    name: str
    what: str
    key: tuple
    grid: str
    check: str
    k: tuple = (0.0, 0.0, 0.0, 0.0)
    j: float = 0.0
    counts: tuple = (255,)
    timed: bool = False
    texture: bool = False

    @property
    def vertex(self):
        return self.key[1]

    @property
    def breaks(self):
        return self.key[0] == "counter" or self.key[0] == "gated" and self.key[2]


GRID_K = (1.0, 10.0, 1.0, 0.0)
NEVER = 256.0
COUNTER = ("counter", False)
DRAWS = [
    Draw("1", "a pass counter left ungated, LOOP_END breaking on each pixel's p0", COUNTER, "GRID_PIXELS",
         "CHECK_COUNTER", GRID_K),
    Draw("2a", "the design's gated values, LOOP_END breaking once no pixel is alive", ("gated", False, True),
         "GRID_PIXELS", "CHECK_GATED", GRID_K),
    Draw("2b", "the same gated values, no break: every pass of 255", ("gated", False, False), "GRID_PIXELS",
         "CHECK_GATED", GRID_K),
    Draw("3a", "timed, a count of 1, every pixel leaving at pass 1", COUNTER, "GRID_PIXELS", "CHECK_COUNTER",
         (0.0, 0.0, 0.0, 1.0), counts=(1,), timed=True),
    Draw("3b", "timed, a count of 255, every pixel leaving at pass 1", COUNTER, "GRID_PIXELS", "CHECK_COUNTER",
         (0.0, 0.0, 0.0, 1.0), timed=True),
    Draw("3c", "timed, every pixel leaving at pass 128", COUNTER, "GRID_PIXELS", "CHECK_COUNTER",
         (0.0, 0.0, 0.0, 128.0), timed=True),
    Draw("3d", "timed, no pixel leaving: 255 passes", COUNTER, "GRID_PIXELS", "CHECK_COUNTER",
         (0.0, 0.0, 0.0, NEVER), timed=True),
    Draw("3e", "timed, half and half in every 2x2 pixel quad: pass 1 where px + py is even, never where odd",
         COUNTER, "GRID_PIXELS", "CHECK_COUNTER", (0.0, 0.0, 0.0, 1.0), NEVER - 1.0, timed=True),
    Draw("3f", "timed, thin triangles filling vectors in part, every pixel leaving at pass 1", COUNTER, "GRID_THIN",
         "CHECK_NONE", (0.0, 0.0, 0.0, 1.0), timed=True),
    Draw("3g", "timed, the thin triangles, no pixel leaving", COUNTER, "GRID_THIN", "CHECK_NONE",
         (0.0, 0.0, 0.0, NEVER), timed=True),
    Draw("4a", "nested 2 deep, counts 3 and 5, a counter at each level", ("nested", False, 2, False), "GRID_PIXELS",
         "CHECK_FIXED", counts=(3, 5)),
    Draw("4b", "nested 3 deep, counts 3, 4 and 5", ("nested", False, 3, False), "GRID_PIXELS", "CHECK_FIXED",
         counts=(3, 4, 5)),
    Draw("4c", "nested 4 deep, counts 2, 3, 4 and 5", ("nested", False, 4, False), "GRID_PIXELS", "CHECK_FIXED",
         counts=(2, 3, 4, 5)),
    Draw("4d", f"nested 2 deep, counts 3 and 5, the inner loop broken out of after {INNER_PASSES} passes",
         ("nested", False, 2, True), "GRID_PIXELS", "CHECK_FIXED", counts=(3, 5)),
    Draw("5a", "a0 loaded and read in the pass, a count of 3", ("a0", False, "in"), "GRID_PIXELS", "CHECK_ROW",
         counts=(3,)),
    Draw("5b", "a0 loaded before LOOP_START and read in the pass, a count of 3", ("a0", False, "before"),
         "GRID_PIXELS", "CHECK_ROW", counts=(3,)),
    Draw("5c", "as 5b, a count of 1", ("a0", False, "before"), "GRID_PIXELS", "CHECK_ROW", counts=(1,)),
    Draw("5d", "a0 loaded in the pass and read after LOOP_END, a count of 3", ("a0", False, "after"), "GRID_PIXELS",
         "CHECK_ROW", counts=(3,)),
    Draw("7a", "a sample in each of 4 passes at the LOD the pass gives through setTexLOD",
         ("sample", False, False, False, True), "GRID_PIXELS", "CHECK_FIXED", counts=(4,), texture=True),
    Draw("7b", "a sample in each of 4 passes at the computed LOD, the coordinates scaled by 2 each pass",
         ("sample", False, True, False, True), "GRID_PIXELS", "CHECK_FIXED", counts=(4,), texture=True),
    Draw("7c", "no loop, one sample at the level setTexLOD gives from the quad's base, mod 4",
         ("level", False, 2, "both"), "GRID_PIXELS", "CHECK_LEVEL_CELL", texture=True),
    Draw("7d", "no loop, one sample at the level setTexLOD gives from the pixel's column, mod 4",
         ("level", False, 0, "both"), "GRID_PIXELS", "CHECK_LEVEL_PIXEL", texture=True),
    Draw("7e", "as 7a, each pass's level kept as a base-4 digit, 0123 wanted", ("sample", False, False, True, True),
         "GRID_PIXELS", "CHECK_FIXED", counts=(4,), texture=True),
    Draw("7f", "as 7c, the fetch after setTexLOD not serialized, as xost leaves it", ("level", False, 2, "free"),
         "GRID_PIXELS", "CHECK_LEVEL_CELL", texture=True),
    Draw("7g", "as 7f, setTexLOD and the fetch alone in an exec after the one working the LOD out",
         ("level", False, 2, "alone"), "GRID_PIXELS", "CHECK_LEVEL_CELL", texture=True),
    Draw("7h", "as 7c, the level from setGradientH and setGradientV, both serialized",
         ("level", False, 2, "gradients"), "GRID_PIXELS", "CHECK_LEVEL_CELL", texture=True),
    Draw("7i", "as 7h, setGradientV and the fetch not serialized, as xost leaves textureGrad",
         ("level", False, 2, "gradients_first"), "GRID_PIXELS", "CHECK_LEVEL_CELL", texture=True),
    Draw("7j", "as 7e, the fetch after setTexLOD not serialized, as xost leaves it",
         ("sample", False, False, True, False), "GRID_PIXELS", "CHECK_FIXED", counts=(4,), texture=True),
    Draw("6a", "vertex shader, today's uniform break: every vertex leaving at pass 7", ("counter", True),
         "GRID_VERTICES", "CHECK_COUNTER", (0.0, 0.0, 0.0, 7.0)),
    Draw("6b", "vertex shader, the gated values and a divergent break: each quad leaving at its own pass",
         ("gated", True, True), "GRID_VERTICES", "CHECK_GATED", (1.0, 0.0, 0.0, 0.0)),
    Draw("6c", "vertex shader, nested 2 deep, counts 3 and 5", ("nested", True, 2, False), "GRID_VERTICES",
         "CHECK_FIXED", counts=(3, 5)),
    Draw("6d", "vertex shader, nested 4 deep, counts 2, 3, 4 and 5", ("nested", True, 4, False), "GRID_VERTICES",
         "CHECK_FIXED", counts=(2, 3, 4, 5)),
]
# Cells and places in them the simulator runs each draw at: corners, edges, both parities.
SAMPLE_CELLS = ((0, 0), (127, 71), (60, 33), (9, 50))
SAMPLE_PLACES = ((0, 0), (9, 9), (3, 7), (8, 1), (5, 5), (9, 0), (0, 9), (4, 6))


def cell_base(column, row):
    return 1 + (7 * column + 13 * row) % 101


def quad_break(cell):
    return 1 + (37 * cell) % 200


def fill(n):
    """c[n]: n, 255 - n and n ^ CHECK, each a quarter into its 8-bit step, and w 1."""
    return [simulate.single((v + 0.25) / 255) for v in (n, 255 - n, n ^ CHECK)] + [1.0]


def constants(draw):
    rows = {n: fill(n) for n in range(FILL_ROWS)}
    rows.update({ROW[name]: [simulate.single(v) for v in values] for name, values in LITERALS})
    rows[ROW_K] = list(draw.k)
    rows[ROW_J] = [2.0 * draw.j] * 4
    return rows


def texel(level):
    """Level L's colour: red 16 + 32L, green 1 << L, blue L ^ CHECK, alpha 255."""
    return 16 + 32 * level, 1 << level, level ^ CHECK, 255


def to_byte(value):
    return int(min(max(value, 0.0), 1.0) * 255.0 + 0.5)


def run(draw, words, sample_point):
    """The draw's export at a sample point as bytes red, green, blue and alpha, and the point's B."""
    blob = struct.pack(f">{len(words)}I", *words)
    first = VERTEX_FIRST_LOOP if draw.vertex else PIXEL_FIRST_LOOP
    loops = {first + level: count for level, count in enumerate(draw.counts)}
    (column, row), (px, py) = sample_point
    if draw.vertex:
        b = quad_break(row * COLUMNS + column)
        inputs = [[0.25, -0.5, 0.5, 1.0], [float(b), 0.0, 0.0, 1.0]]
        result = simulate.run(blob, {0: [0.0] * 4}, constants(draw), loops=loops,
                              vertex_fetch=lambda n, fetch, index: inputs[n])
        if result.exports.get(tri.EXPORT_POSITION) != inputs[0]:
            sys.exit(f"gen_loop_break_probe: {draw.name}: the position export is wrong")
        point_break = draw.k[0] * b + draw.k[3]
    else:
        base = cell_base(column, row)
        u = px + 0.5

        def texture_lod(slot, coordinates, lod):
            assert slot == TEXTURE_SLOT
            level = (lod.register or 0.0) + lod.bias
            if lod.gradients:
                level += math.log2(max(math.hypot(*g[:2]) for g in lod.gradients) * TEXTURE_SIZE)  # 2D: x and y
            elif lod.computed:
                level += math.log2(coordinates[0] / u * TEXTURE_SIZE)
            level = min(max(math.floor(level + 0.5), 0), TEXTURE_LEVELS - 1)
            return [simulate.single(c / 255) for c in texel(level)]

        # Xenia's model, which keeps a0 past the colours' alloc: the noa0 break reads the a0 it starts with.
        result = simulate.run(blob, {INTERP: [u, py + 0.5, base + 0.5, 1.5]}, constants(draw), loops=loops,
                              texture_lod=texture_lod, keep_a0=True)
        point_break = (draw.k[0] * px + draw.k[1] * py + draw.k[2] * base + draw.k[3] +
                       (draw.j if (px + py) % 2 else 0.0))
    value = result.exports.get(tri.EXPORT_COLOR_0)
    if value is None:
        sys.exit(f"gen_loop_break_probe: {draw.name}: no export")
    return [to_byte(v) for v in value], int(point_break), base if not draw.vertex else None, px


def stored(rgba):
    """What a resolve stores: blue, green, red, alpha from the top byte (tests/probe_constants.c)."""
    r, g, b, a = rgba
    return b << 24 | g << 16 | r << 8 | a


def expected(draw, b, base, px, fixed):
    """The bytes the probe's check wants under Xenia's model, each pixel leaving at its own pass, or None for any."""
    count = draw.counts[0]
    if draw.check in ("CHECK_LEVEL_CELL", "CHECK_LEVEL_PIXEL"):
        return list(texel((base if draw.check == "CHECK_LEVEL_CELL" else px) % SET_LEVELS))
    if draw.check in ("CHECK_COUNTER", "CHECK_NONE"):
        return [min(b, count), min(b, 255), CHECK, 255]
    if draw.check == "CHECK_GATED":
        return [b, b + GATED_W, b, b if draw.breaks else count]
    if draw.check == "CHECK_ROW":
        return fill_bytes(A0_ROW + base)
    return fixed


def fill_bytes(row):
    return [row, 255 - row, row ^ CHECK, 255]


def model_fixed(draw):
    """A fixed draw's bytes as worked out by hand: the counters' products, or the last pass's level and the levels
    sampled."""
    if draw.key[0] == "sample":
        r, _, b, a = texel(draw.counts[0] - 1)
        levels = range(draw.counts[0])
        return [r, sum(level * 4 ** (len(levels) - 1 - level) for level in levels) if draw.key[3] else
                sum(1 << level for level in levels), b, a]
    if draw.key[3]:
        outer, inner = draw.counts
        return [outer, outer * min(inner, INNER_PASSES), 0, 0]
    products = [math.prod(draw.counts[:level + 1]) for level in range(len(draw.counts))]
    return products + [0] * (4 - len(products))


def check_draw(draw, words):
    """Stops unless the simulator gives every sample point the bytes the check wants."""
    fixed = model_fixed(draw) if draw.check == "CHECK_FIXED" else None
    for point in ((c, p) for c in SAMPLE_CELLS for p in SAMPLE_PLACES):
        got, b, base, px = run(draw, words, point)
        want = expected(draw, b, base, px, fixed)
        if got != want:
            sys.exit(f"gen_loop_break_probe: {draw.name} at cell {point[0]}, place {point[1]}: the simulator gives "
                     f"{got}, the check wants {want}")
    return fixed


def fails(draw, words):
    """Whether the simulator shows the check failing at some sample point."""
    fixed = model_fixed(draw) if draw.check == "CHECK_FIXED" else None
    for point in ((c, p) for c in SAMPLE_CELLS for p in SAMPLE_PLACES):
        got, b, base, px = run(draw, words, point)
        want = expected(draw, b, base, px, fixed)
        if draw.check == "CHECK_GATED":
            got, want = got[:3], want[:3]
        if got != want:
            return True
    return False


def check_encodings():
    """Stops unless the loop builders write xost's words for vk_loop_count_if.frag's loop and its predicated break."""
    spirv = (ROOT / "xenos-toolchain" / "xsc" / "testdata" / "vk_loop_count_if.frag.spv").read_bytes()
    blob = native.compile_spirv(spirv, pack.REGISTERS, pack.REGISTERS)[0]
    parsed = container.Container(blob)
    seen = set()
    for rec in xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0]:
        d = rec.detail if rec.kind == "cf" else None
        if isinstance(d, xu.ParsedLoopStartInstruction):
            ours = enc.build_cf_loop_start(address=d.loop_skip_address, loop_id=d.loop_constant_index)
        elif isinstance(d, xu.ParsedLoopEndInstruction):
            ours = enc.build_cf_loop_end(address=d.loop_body_address, loop_id=d.loop_constant_index,
                                         is_predicated_break=d.is_predicated_break, condition=d.predicate_condition)
            seen.add("break" if d.is_predicated_break else "end")
        else:
            continue
        seen.add(type(d).__name__)
        if tuple(ours) != tuple(rec.raw):
            sys.exit(f"gen_loop_break_probe: the builders don't write xost's {type(d).__name__}")
    if not {"ParsedLoopStartInstruction", "break"} <= seen:
        sys.exit("gen_loop_break_probe: vk_loop_count_if.frag has no loop with a predicated break to check against")


def c_float(value):
    return f"{value!r}f" if value != int(value) else f"{value:.1f}f"


def shader_lines(index, variants):
    """The shader's words, a break build's own where it differs."""
    normal = variants[None]
    lines = []
    changed = [(brk, words) for brk, words in variants.items() if brk and words != normal]
    for n, (brk, words) in enumerate(changed):
        lines += [f"#{'el' if n else ''}if defined(LOOP_BREAK_{brk.upper()})",
                  f"static const uint32_t shader_{index}[] = {{", pack.word_lines(words), "};"]
    lines += (["#else"] if changed else []) + [f"static const uint32_t shader_{index}[] = {{", pack.word_lines(normal),
                                               "};"] + (["#endif"] if changed else []) + [""]
    return lines


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    check_encodings()
    keys = list(dict.fromkeys(draw.key for draw in DRAWS))
    variants = {key: {brk: shader(key, brk) for brk in (None, *BREAKS)} for key in keys}
    lines = ["// Generated by tools/gen_loop_break_probe.py; don't edit.", "",
             f"enum {{ PROBE_FILL_ROWS = {FILL_ROWS}, PROBE_A0_ROW = {A0_ROW}, PROBE_ROW_K = {ROW_K}, "
             f"PROBE_ROW_J = {ROW_J}, PROBE_LITERAL_FIRST = {LITERAL_FIRST}, PROBE_LITERALS = {len(LITERALS)}, "
             f"PROBE_GATED_W = {GATED_W}, PROBE_TEXTURE_SLOT = {TEXTURE_SLOT} }};", ""]
    lines += ["static const float probe_literals[PROBE_LITERALS][4] = {"]
    lines += ["\t{ " + ", ".join(c_float(simulate.single(v)) for v in values) + " }," for _, values in LITERALS]
    lines += ["};", ""]
    failing = {brk: [] for brk in BREAKS}
    table = []
    for draw in DRAWS:
        index = keys.index(draw.key)
        fixed = check_draw(draw, variants[draw.key][None])
        for brk in BREAKS:
            if variants[draw.key][brk] != variants[draw.key][None] and fails(draw, variants[draw.key][brk]):
                failing[brk].append(draw.name)
        counts = list(draw.counts) + [0] * (4 - len(draw.counts))
        word = stored(fixed) if fixed else 0
        table.append(f'\t{{ "{draw.name}", "{draw.what}", shader_{index}, sizeof shader_{index}, '
                     f"{str(draw.vertex).lower()}, {str(draw.breaks).lower()}, {draw.grid}, {draw.check}, "
                     f"{{ {', '.join(c_float(k) for k in draw.k)} }}, {c_float(draw.j)}, "
                     f"{{ {', '.join(map(str, counts))} }}, 0x{word:08X}, {str(draw.timed).lower()}, "
                     f"{str(draw.texture).lower()} }},")
    for brk, names in failing.items():
        if not names:
            sys.exit(f"gen_loop_break_probe: the {brk} break build fails no check on the simulator")
    for index, key in enumerate(keys):
        lines += shader_lines(index, variants[key])
    for n, (brk, what) in enumerate(BREAKS.items()):
        lines += [f"#{'el' if n else ''}if defined(LOOP_BREAK_{brk.upper()})",
                  f'static const char probe_break[] = "{what}; on the simulator it fails {", ".join(failing[brk])}";']
    lines += ["#else", "static const char probe_break[] = \"\";", "#endif", ""]
    lines += ["static const ProbeDraw probe_draws[] = {"] + table + ["};", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
