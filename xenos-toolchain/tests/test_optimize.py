#!/usr/bin/env python3
"""The compiler's optimiser (xsdk:src/xsc_optimize.c), case by case, on hand-built pixel programs the C builds as it
builds the compiler's (tests/xsc_internals.py).

Each case builds a program twice, without and with the pass, and checks:
  - the two microcodes compute the same exports, run on random inputs and constants by xdis/equivalence.py, on the
    components the first defines;
  - the pass left the number of ALU instructions the case expects: what it may remove it does, and what it mustn't
    touch (a move other values share a register with, a source changed in between, a third constant) it keeps, an
    op that writes one value co-issued as a scalar op beside a vector one where it can.
The fetch cases check fetches the same way, and two testdata shaders what a dropped fetch leaves: a vertex shader's
elements, one for each fetch left, and a pixel shader's sampler. A last case hands the comparison a wrong rewrite, to
show it would see one. Runs in WSL, where make builds build/host/libxsc.so.

tools/check_optimize.py runs the same comparison over a folder of real shaders.
"""
import math
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

sys.path.insert(0, str(ROOT / "tests"))

import xasm.program as program  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.equivalence as equivalence  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import _templates  # noqa: E402
from xsc_internals import ABS_SOURCE, pixel_program  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
T = _templates()
INPUTS = 2                                   # r0 and r1 are interpolators; temps start at r2


def r(reg, swizzle="xyzw", negate=False, absolute=False):
    return ("r", reg | (ABS_SOURCE if absolute else 0), swizzle, negate)


def c(reg, swizzle="xyzw", negate=False):
    return ("c", reg, swizzle, negate)


def alu(vop, dest, mask, s1, s2=None, s3=None, sop=S.kRetainPrev, sdest=0, smask=0, export=False, clamp=False):
    f = dict(T["mov"], vector_opcode=vop, vector_dest=dest, vector_write_mask=mask, vector_clamp=clamp,
             scalar_opcode=sop, scalar_dest=sdest, scalar_write_mask=smask, scalar_clamp=False, is_export=export)
    for n, s in ((1, s1), (2, s2 or s1), (3, s3 or s1)):
        kind, reg, swizzle, negate = s
        f[f"src{n}_reg"] = reg
        f[f"src{n}_is_temp"] = kind == "r"
        f[f"src{n}_swizzle"] = program.swizzle4(swizzle)
        f[f"src{n}_negate"] = negate
    return f


def move(dest, src, mask=0xF):
    return alu(V.kMax, dest, mask, src, src)


def scalar(sop, dest, src, mask=0xF):
    """A scalar op alone, as xsc's emit_scalar builds it."""
    f = alu(V.kMax, 0, 0, src, src, src, sop=sop, sdest=dest, smask=mask)
    return f


def export(src):
    return alu(V.kMax, 0, 0xF, src, src, export=True)


def indexed(dest, reg):
    """A copy of the constant a0 past reg, as xsc's block lowering builds one (xsdk_xsc_block_register)."""
    return dict(move(dest, c(reg)), is_const_address_register_relative=True, src1_const_rel=True, src2_const_rel=True)


def at_a0(f):
    """ALU fields f with operand 1, a constant, read at a0."""
    return dict(f, is_const_address_register_relative=True, src1_const_rel=True)


def set_a0(src):
    """a0 from src's x, floored, as xsc's set_a0 loads it."""
    return scalar(S.kMaxAsf, 0, src, 0)


def build(body, exports, optimize):
    return pixel_program(INPUTS, ce.USAGE_TEXCOORD, body, exports, XSDK_REGISTERS.pixel, optimize)


def alu_count(blob):
    cont = container.Container(blob)
    return sum(1 for rec in xu._decode_program(cont.ucode_dwords, cont.shader_type)[0] if rec.kind == "alu")


def differ(a, b):
    if a is None or b is None:
        return a is not b
    return a != b and not (math.isnan(a) and math.isnan(b))


def compare(plain, optimized, trials=8):
    """What differs between the two microcodes' exports, if anything, on the components the first defines."""
    rng = random.Random(7)
    for trial in range(trials):
        registers = {i: [rng.uniform(-3.0, 3.0) for _ in range(4)] for i in range(INPUTS)}
        constants = {i: [rng.uniform(-3.0, 3.0) for _ in range(4)] for i in range(16)}
        want = equivalence.run(plain, registers, constants)
        got = equivalence.run(optimized, registers, constants)
        for reg in want:
            for i in range(4):
                if want[reg][i] is not None and differ(want[reg][i], got.get(reg, [None] * 4)[i]):
                    return f"trial {trial}: export {reg}.{'xyzw'[i]} {want[reg][i]} -> {got[reg][i]}"
    return None


def case(name, body, exports, expect_alu):
    plain = build(body, exports, False)
    optimized = build(body, exports, True)
    problem = compare(plain, optimized)
    count = alu_count(optimized)
    ok = problem is None and count == expect_alu
    detail = problem or f"{alu_count(plain)} -> {count} ALU instructions, {expect_alu} expected"
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")
    return ok


CASES = [
    ("a move's readers read its source, and the move goes",
     [move(2, r(0, "yxwz")), alu(V.kAdd, 3, 0xF, r(2), c(0))],
     [export(r(3))], 2),
    ("a negated move from a constant folds into its reader",
     [move(2, c(1, negate=True)), alu(V.kMul, 3, 0xF, r(2), r(0))],
     [export(r(3))], 2),
    ("an absolute move from a temp folds, its reader keeping the absolute value",
     [move(2, r(0, absolute=True)), alu(V.kAdd, 3, 0xF, r(2, "wzyx", negate=True), r(1))],
     [export(r(3))], 2),
    ("a move from a constant whose reader takes an absolute value stays",
     [move(2, c(1)), alu(V.kAdd, 3, 0xF, r(2, absolute=True), r(1))],
     [export(r(3))], 3),
    ("a vector assembled by two moves stays, as no one source holds it",
     [move(2, r(0), 0x3), move(2, c(0, "xyzz"), 0x4), alu(V.kDp3, 3, 0xF, r(2), r(1))],
     [export(r(3))], 3),
    ("a move whose source changes before its reader stays",
     [move(2, r(0)), alu(V.kAdd, 0, 0xF, r(1), c(0)), alu(V.kMul, 3, 0xF, r(2), r(0))],
     [export(r(3))], 4),
    ("a recomputed sum is the first one's",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(0), c(0)), alu(V.kMul, 4, 0xF, r(2), r(3))],
     [export(r(4))], 3),
    ("a sum of different values isn't merged",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(0), c(1)), alu(V.kMul, 4, 0xF, r(2), r(3))],
     [export(r(4))], 4),
    ("mul then add is a mad, the product's negation moving to a factor",
     [alu(V.kMul, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(2, "zwxy", negate=True), r(1))],
     [export(r(3))], 2),
    ("a product read twice isn't fused",
     [alu(V.kMul, 2, 0xF, r(0), c(0)), alu(V.kAdd, 3, 0xF, r(2), r(1)), alu(V.kAdd, 4, 0xF, r(3), r(2))],
     [export(r(4))], 4),
    ("a scalar op joins an independent vector op",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), scalar(S.kRcp, 3, r(1, "xyzw"), 0x1),
      alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 3),
    ("a scalar op reading the vector op's result doesn't join it",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), scalar(S.kRsq, 3, r(2, "xyzw"), 0x1),
      alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 4),
    ("a scalar op moves down past a mad to a vector op that reads the mad's result",
     [scalar(S.kRcp, 3, r(1, "xyzw"), 0x1), alu(V.kMad, 2, 0xF, r(0), c(0), r(1)),
      alu(V.kMul, 4, 0xF, r(2), c(1)), alu(V.kMul, 5, 0xF, r(4), r(3, "xxxx"))],
     [export(r(5))], 4),
    ("a vector op moves down past a mad to a scalar op that reads the mad's result",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kMad, 3, 0xF, r(1), c(1), r(1)), scalar(S.kRsq, 4, r(3, "xyzw"), 0x1),
      alu(V.kMul, 5, 0xF, r(2), r(4, "xxxx"))],
     [export(r(5))], 4),
    ("a scalar op doesn't move down past an op that writes what it reads",
     [alu(V.kMul, 6, 0xF, r(0), c(2)), scalar(S.kRcp, 3, r(6, "xyzw"), 0x1), alu(V.kMad, 6, 0xF, r(1), c(0), r(1)),
      alu(V.kMul, 4, 0xF, r(6), c(1)), alu(V.kMul, 5, 0xF, r(4), r(3, "xxxx"))],
     [export(r(5))], 6),
    ("a copy of a constant read at a0 goes, its reader reading it at a0",
     [set_a0(r(0, "xxxx")), indexed(2, 4), scalar(S.kRcp, 3, r(1, "xyzw"), 0x1),
      alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 4),
    ("a scalar op reading a constant doesn't join a vector op reading one at a0, though their registers match",
     [set_a0(r(0, "xxxx")), indexed(2, 4), scalar(S.kRcp, 3, c(4), 0x1), alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 4),
    ("nothing moves past the op that loads a0",
     [set_a0(r(0, "xxxx")), scalar(S.kRcp, 3, r(1, "xyzw"), 0x1), set_a0(r(0, "yyyy")), indexed(2, 4),
      alu(V.kMul, 4, 0xF, r(2), r(3, "xxxx"))],
     [export(r(4))], 5),
    ("a copy of a constant read at a0 stays where a0 is loaded again before its reader",
     [set_a0(r(0, "xxxx")), indexed(2, 4), set_a0(r(0, "yyyy")), alu(V.kMul, 3, 0xF, r(2), r(1))],
     [export(r(3))], 5),
    ("a copy of a constant read at a0 stays where an alloc, which loses a0, comes before its reader",
     [set_a0(r(0, "xxxx")), indexed(2, 4)],
     [alu(V.kMul, 0, 0xF, r(2), r(1), export=True)], 3),
    ("two copies of a constant read at one a0 go, their reader reading it twice",
     [set_a0(r(0, "xxxx")), indexed(2, 4), indexed(3, 4), alu(V.kAdd, 4, 0xF, r(2), r(3, "yxwz"))],
     [export(r(4))], 3),
    ("a product of a constant read at a0 fuses with the add after it",
     [set_a0(r(0, "xxxx")), indexed(2, 4), alu(V.kMul, 3, 0xF, r(2), r(0)), alu(V.kAdd, 4, 0xF, r(3), r(1))],
     [export(r(4))], 3),
    ("of reads at two indices, the one a0 still holds goes into its reader",
     [set_a0(r(0, "xxxx")), indexed(2, 4), set_a0(r(0, "yyyy")), indexed(3, 6), alu(V.kAdd, 4, 0xF, r(2), r(3))],
     [export(r(4))], 5),
    ("a product of a constant read at a0 isn't computed into its export past an a0 load",
     [],
     [set_a0(r(0, "xxxx")), at_a0(alu(V.kMul, 2, 0xF, c(4), r(1))), set_a0(r(0, "yyyy")), export(r(2))], 4),
    ("a constant read at a0 doesn't join a plain read of its register in the slot they'd share",
     [set_a0(r(0, "xxxx")), indexed(2, 5), alu(V.kMad, 3, 0xF, c(6), c(5), r(2))],
     [export(r(3))], 4),
    ("a sum of a constant read at a0 isn't the same sum of its register read plainly",
     [set_a0(r(0, "xxxx")), alu(V.kAdd, 2, 0xF, c(4), r(1)), at_a0(alu(V.kAdd, 3, 0xF, c(4), r(1))),
      alu(V.kMul, 4, 0xF, r(2), r(3))],
     [export(r(4))], 5),
    ("a third constant isn't folded into an instruction holding two",
     [move(2, c(2)), alu(V.kMad, 3, 0xF, c(0), c(1), r(2))],
     [export(r(3))], 3),
    ("dead code goes",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), alu(V.kMul, 3, 0xF, r(1), c(1))],
     [export(r(3))], 2),
    ("components computed apart and copied into one vector are computed there, a sum swizzled to its lane",
     [alu(V.kDp4, 2, 0x1, r(0), c(0)), alu(V.kAdd, 3, 0x1, r(0, "zzzz"), c(1)), move(4, r(2), 0x1),
      move(4, r(3, "xxxx"), 0x2), alu(V.kMul, 5, 0xF, r(4, "xyxy"), r(1))],
     [export(r(5))], 3),
    ("a copy clamped to 0 to 1 becomes its writer's clamp",
     [alu(V.kMad, 2, 0xF, r(0), c(0), r(1)), alu(V.kMax, 3, 0xF, r(2), r(2), clamp=True),
      alu(V.kMul, 4, 0xF, r(3), c(1))],
     [export(r(4))], 3),
    ("a clamp isn't put on a result something else reads unclamped",
     [alu(V.kMad, 2, 0xF, r(0), c(0), r(1)), alu(V.kMax, 3, 0xF, r(2), r(2), clamp=True),
      alu(V.kMul, 4, 0xF, r(3), r(2))],
     [export(r(4))], 4),
    ("a dot product's other reader reads it from the copy's place",
     [alu(V.kDp3, 2, 0x3, r(0), r(1)), move(3, r(2), 0x1), move(3, r(1), 0x2), scalar(S.kRsq, 4, r(2, "yyyy"), 0x1),
      alu(V.kMul, 5, 0xF, r(3, "xyxy"), r(4, "xxxx"))],
     [export(r(5))], 4),
    ("a product whose other lane is read elsewhere stays in its temp",
     [alu(V.kMul, 2, 0x3, r(0), c(0)), move(3, r(2), 0x1), move(3, r(1), 0x2),
      alu(V.kAdd, 4, 0xF, r(3, "xyxy"), r(2, "yyyy"))],
     [export(r(4))], 4),
    ("a component whose place is read before the copy stays in its temp",
     [move(3, r(1), 0x7), alu(V.kAdd, 3, 0x8, r(0), c(2)), alu(V.kAdd, 2, 0x1, r(0), c(0)),
      alu(V.kMul, 4, 0xF, r(3), c(1)), move(3, r(2, "xxxx"), 0x1), alu(V.kAdd, 5, 0xF, r(3), r(4))],
     [export(r(5))], 6),
    ("an export assembled from parts takes each part, its lanes nothing writes filled by one",
     [],
     [alu(V.kDp4, 2, 0x1, r(0), c(0)), alu(V.kMul, 3, 0x1, r(1, "yyyy"), c(1)), move(4, r(2), 0x1),
      move(4, r(3, "xxxx"), 0x2), alu(V.kMax, 0, 0xF, r(4), r(4), export=True)], 2),
    ("a value computed before the colours' alloc is copied to its export after it",
     [alu(V.kAdd, 2, 0xF, r(0), c(0))],
     [export(r(2))], 2),
    ("copies taken in one scan, the last of what the first two put in place",
     [alu(V.kDp4, 2, 0x1, r(0), c(0)), scalar(S.kExp, 3, r(0, "zzzz"), 0x1), move(4, r(2), 0x1),
      move(4, r(3, "xxxx"), 0x2), move(5, r(4), 0x3), alu(V.kAdd, 5, 0x4, r(1, "xxxx"), c(2)),
      alu(V.kMul, 6, 0xF, r(5, "xyzx"), r(1))],
     [export(r(6))], 4),
    ("three dot products copied into one vector, and read together elsewhere, are computed there",
     [alu(V.kDp4, 2, 0x1, r(0), c(0)), alu(V.kDp4, 2, 0x2, r(0), c(1)), alu(V.kDp4, 2, 0x4, r(0), c(2)),
      move(3, r(2), 0x7), move(3, c(3), 0x8), alu(V.kDp4, 4, 0x1, r(3), c(4)),
      alu(V.kAdd, 5, 0xF, c(5), r(2, "yzxy", negate=True)), alu(V.kMul, 6, 0xF, r(5), r(4, "xxxx"))],
     [export(r(6))], 7),
    ("an add's lanes copied one at a time into one vector are computed there, swizzled to their lanes",
     [move(3, c(3), 0x8), alu(V.kAdd, 2, 0x7, r(0), c(0)), move(3, r(2, "zzzz"), 0x1), move(3, r(2, "xxxx"), 0x2),
      move(3, r(2, "yyyy"), 0x4), alu(V.kMul, 4, 0xF, r(3), r(1))],
     [export(r(4))], 3),
    ("an add's lanes copied one at a time stay where one's place is read before its copy",
     [move(3, c(3)), alu(V.kAdd, 2, 0x3, r(0), c(0)), move(3, r(2), 0x1), alu(V.kMul, 4, 0xF, r(3), r(1)),
      move(3, r(2), 0x2), alu(V.kAdd, 5, 0xF, r(3), r(4))],
     [export(r(5))], 5),
    ("a row moved twice from the same constants takes one dot product",
     [move(2, c(0), 0x3), move(2, c(1, "xxxx"), 0x4), move(3, c(0), 0x3), move(3, c(1, "xxxx"), 0x4),
      alu(V.kDp3, 4, 0x1, r(2), r(0)), alu(V.kDp3, 5, 0x1, r(3), r(0)),
      alu(V.kMul, 6, 0xF, r(4, "xxxx"), r(5, "xxxx"))],
     [export(r(6))], 5),
    ("a row moved from negated constants isn't the row's",
     [move(2, c(0), 0x3), move(2, c(1, "xxxx"), 0x4), move(3, c(0, negate=True), 0x3), move(3, c(1, "xxxx"), 0x4),
      alu(V.kDp3, 4, 0x1, r(2), r(0)), alu(V.kDp3, 5, 0x1, r(3), r(0)),
      alu(V.kMul, 6, 0xF, r(4, "xxxx"), r(5, "xxxx"))],
     [export(r(6))], 7),
]


def tfetch(dest, src, **fields):
    return dict(T["tf"], dest=dest, src=src, **fields)


# A fetch goes when nothing live reads what it writes. setTexLOD and the gradients' setters write state, and they and
# the getters stay; a kill or a setp reading a fetch keeps it, as they keep any op they read.
FETCH_CASES = [
    ("a texture fetch nothing reads goes, and the coordinate work only it needed",
     [alu(V.kAdd, 2, 0xF, r(0), c(0)), tfetch(3, 2), alu(V.kMul, 4, 0xF, r(1), c(1))],
     [export(r(4))], 0, 2, True),
    ("a texture fetch read in one lane stays",
     [tfetch(2, 0), alu(V.kMul, 3, 0x1, r(2, "wwww"), c(0)), alu(V.kAdd, 4, 0xF, r(1), r(3, "xxxx"))],
     [export(r(4))], 1, 2, True),
    ("a texture fetch only a kill reads stays",
     [tfetch(2, 0), alu(V.kKillGt, 0, 0, r(2), c(0)), alu(V.kMul, 3, 0xF, r(1), c(1))],
     [export(r(3))], 1, 3, True),
    ("a texture fetch only a setp reads stays",
     [tfetch(2, 0), scalar(S.kSetpGt, 0, r(2, "xxxx"), 0), alu(V.kMul, 3, 0xF, r(1), c(1))],
     [export(r(3))], 1, 3, False),
    ("setTexLOD stays though the fetch that reads its LOD goes",
     [tfetch(0, 1, opcode=int(xu.FetchOpcode.kSetTextureLod), src_swizzle=0, dest_swizzle=0o7777),
      tfetch(2, 0, use_computed_lod=False, use_register_lod=True), alu(V.kMul, 3, 0xF, r(1), c(1))],
     [export(r(3))], 1, 2, True),
    ("getTextureGradients stays though nothing reads it",
     [tfetch(2, 0, opcode=int(xu.FetchOpcode.kGetTextureGradients)), alu(V.kMul, 3, 0xF, r(1), c(1))],
     [export(r(3))], 1, 2, True),
]


def fetch_count(blob):
    cont = container.Container(blob)
    return sum(rec.kind in ("tfetch", "vfetch") for rec in xu._decode_program(cont.ucode_dwords, cont.shader_type)[0])


def fetch_case(name, body, exports, expect_fetches, expect_alu, simulated):
    """simulated is False for a setp, which equivalence.py doesn't run."""
    plain = build(body, exports, False)
    optimized = build(body, exports, True)
    problem = compare(plain, optimized) if simulated else None
    got = (fetch_count(optimized), alu_count(optimized))
    ok = problem is None and got == (expect_fetches, expect_alu)
    seen = f"{problem}; " if problem else ""
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {seen}{fetch_count(plain)} -> {got[0]} fetches, "
          f"{alu_count(plain)} -> {got[1]} ALU instructions, {expect_fetches} and {expect_alu} expected")
    return ok


def compiled(name, optimize):
    return native.compile_spirv((ROOT / "xsc" / "testdata" / name).read_bytes(), XSDK_REGISTERS.pixel,
                                XSDK_REGISTERS.vertex, optimize)


def vertex_elements(blob):
    """A vertex shader's elements, (slot, usage, usage index) each, which the runtime patches its fetches by
    (xsdk:src/shader.c, xsdk_shader_set_vertex_layout): the record's count, then the elements four words on."""
    words = struct.unpack(f">{len(blob) // 4}I", blob)
    record = words[6] // 4
    return [(w & 0xFFF, w >> 12 & 0xF, w >> 16 & 0xF) for w in words[record + 10:record + 10 + words[record + 7]]]


def unused_inputs_lose_their_elements():
    """cubedir.vert reads a_position alone: optimised, a_color's and a_texcoord's fetches go with their elements, and
    the element left names its fetch's slot."""
    plain, optimized = (compiled("cubedir.vert.spv", o)[0] for o in (False, True))
    got = []
    for blob in (plain, optimized):
        cont = container.Container(blob)
        slots = [rec.offset for rec in xu._decode_program(cont.ucode_dwords, "vertex")[0] if rec.kind == "vfetch"]
        elements = vertex_elements(blob)
        got.append((sorted(slots) == sorted(e[0] for e in elements), sorted(e[1] for e in elements)))
    problem = compare(plain, optimized)
    ok = problem is None and got == [(True, [ce.USAGE_POSITION, ce.USAGE_TEXCOORD, ce.USAGE_COLOR]),
                                     (True, [ce.USAGE_POSITION])]
    print(f"  {'PASS' if ok else 'FAIL'} cubedir.vert's unused inputs lose their fetches and elements: "
          f"{problem or got}")
    return ok


def a_dropped_sample_keeps_its_sampler():
    """vk_unused_sample.frag samples and uses none of it: optimised, the fetch goes, and the container's sampler and the
    sampler resource stay, so what a host binds doesn't change."""
    (plain, _, plain_resources), (optimized, _, resources) = (compiled("vk_unused_sample.frag.spv", o)
                                                             for o in (False, True))
    got = (fetch_count(plain), fetch_count(optimized), b"tex0\0" in plain, b"tex0\0" in optimized)
    ok = got == (1, 0, True, True) and resources == plain_resources == [("sampler", 0, 0, 0, 1)]
    print(f"  {'PASS' if ok else 'FAIL'} a dropped sample keeps its sampler: fetches {got[0]} -> {got[1]}, tex0 "
          f"{got[2]} -> {got[3]}, resources {plain_resources} -> {resources}")
    return ok


def wrong_rewrite_is_seen():
    """A rewrite that reads the wrong component must show as a difference."""
    plain = build([move(2, r(0, "yxwz")), alu(V.kAdd, 3, 0xF, r(2), c(0))], [export(r(3))], False)
    wrong = build([alu(V.kAdd, 3, 0xF, r(0), c(0))], [export(r(3))], False)
    problem = compare(plain, wrong)
    print(f"  {'PASS' if problem else 'FAIL'} a wrong rewrite shows: {problem or 'nothing seen'}")
    return problem is not None


def main():
    ok = all([case(*c_) for c_ in CASES]) & all([fetch_case(*c_) for c_ in FETCH_CASES])
    ok = ok & unused_inputs_lose_their_elements() & a_dropped_sample_keeps_its_sampler() & wrong_rewrite_is_seen()
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
