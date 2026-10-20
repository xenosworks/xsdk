"""Runs a Xenos shader program on the CPU, for checking what xsc lowers by its numbers.

The program's control flow runs as Xenia's interpreter runs it for one vertex or pixel
(xenia:src/xenia/gpu/shader_interpreter.cc:26-258): an exec on the predicate runs when p0 is its condition, an
exec that ends the program ends it only when it runs, and a hardware loop runs as many passes as the loop constant
the caller gives says, from aL 0 by 1, a predicated LOOP_END breaking when p0 is its condition; without loop
constants each loop's body runs once. run_group runs several in lockstep, as the GPU runs a group of 64: an exec on
the predicate runs if any of them needs it, each instruction predicated for each, and a predicated LOOP_END breaks
only when every one agrees (xenia:src/xenia/gpu/ucode.h:114-124, 372-373); loops nest 4 deep (ucode.h:131).
The ALU's vector and scalar ops are as Xenia's ucode.h describes them
(xenia:src/xenia/gpu/ucode.h, AluScalarOpcode and AluVectorOpcode), with a temp's absolute-value bit, a constant's,
negation, swizzles, write masks, clamps, a0- and aL-relative constants (MOVA's maxas and maxasf), a0 lost at an
alloc as on the console, where a read at it raises, the predicate and kills, and the cube op as Xenia runs it; every value written is rounded to a single, as the GPU keeps it. Exports land in their export registers.
Vertex and texture fetches come from functions the caller gives; setTexLOD and the gradients' setters keep the state
the texture fetches after them read. A group run as quads, each four lanes a 2x2 quad, gives getGradients and a computed
LOD the coordinates' coarse gradients over the quad, as Xenia's translator takes them
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:595-634), and a fetch at register gradients the top left lane's,
as the console takes one LOD a quad from its first pixel inside the primitive (xsdk:docs/HARDWARE.md, "Samples in a
loop's pass, milestone 46 step 6"); every quad here is whole.
"""
import math
import struct
from dataclasses import dataclass

import xdis.container as container
import xdis.xenos_ucode as xu

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
FLT_MAX = 3.4028234663852886e38
SET_LOD = int(xu.FetchOpcode.kSetTextureLod)
SET_GRADIENTS = (int(xu.FetchOpcode.kSetTextureGradientsHorz), int(xu.FetchOpcode.kSetTextureGradientsVert))
GET_GRADIENTS = int(xu.FetchOpcode.kGetTextureGradients)


@dataclass
class Lod:
    """What a texture fetch's LOD comes from, as Xenia sums it (xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:
    1498-1536): the fetch's own bias, the register LOD if it reads it, and, where it computes the LOD, the coordinate's
    gradients or the registers' (horizontal, vertical), each three floats. The fetch constant's bias is the caller's."""
    computed: bool
    bias: float
    register: float | None
    gradients: tuple | None


def single(x: float) -> float:
    """x rounded to a single, as a register holds it."""
    if math.isnan(x) or math.isinf(x):
        return x
    if abs(x) > FLT_MAX:
        return math.copysign(math.inf, x)
    return struct.unpack("<f", struct.pack("<f", x))[0]


class Result:
    def __init__(self, exports: dict, killed: bool, registers: dict, passes: int = 0):
        self.exports = exports        # export register -> [x, y, z, w]
        self.killed = killed          # a kill passed
        self.registers = registers    # temp -> [x, y, z, w] at the end
        self.passes = passes          # LOOP_ENDs reached, every loop's passes together


def _floor(x):
    return float(math.floor(x)) if math.isfinite(x) else x


def _trunc(x):
    return float(math.trunc(x)) if math.isfinite(x) else x


def _saturate(x):
    """x clamped to 0 to 1; NaN and -0 give +0 (xenia:src/xenia/base/math.h:64-79)."""
    return min(x, 1.0) if x > 0.0 else 0.0


def _cube(s1):
    """The cube op's T, S, twice the major axis and the face, from src1 read .z_xy, as Xenia runs it
    (xenia:src/xenia/gpu/spirv_shader_translator_alu.cc:521-686)."""
    x, y, z = s1[2], s1[3], s1[0]
    if abs(z) >= abs(x) and abs(z) >= abs(y):
        return [-y, -x if z < 0.0 else x, 2.0 * z, 5.0 if z < 0.0 else 4.0]
    if abs(y) >= abs(x):
        return [-z if y < 0.0 else z, x, 2.0 * y, 3.0 if y < 0.0 else 2.0]
    return [-y, z if x < 0.0 else -z, 2.0 * x, 1.0 if x < 0.0 else 0.0]


def _vector(op, s1, s2, s3):
    """A vector op's result, and whether it kills."""
    vo = op.vector_opcode()
    each = range(4)
    if vo == V.kAdd:
        return [s1[i] + s2[i] for i in each], False
    if vo == V.kMul:
        return [s1[i] * s2[i] for i in each], False
    if vo == V.kMax:
        return [s1[i] if s1[i] >= s2[i] else s2[i] for i in each], False
    if vo == V.kMin:
        return [s1[i] if s1[i] < s2[i] else s2[i] for i in each], False
    if vo == V.kSeq:
        return [1.0 if s1[i] == s2[i] else 0.0 for i in each], False
    if vo == V.kSgt:
        return [1.0 if s1[i] > s2[i] else 0.0 for i in each], False
    if vo == V.kSge:
        return [1.0 if s1[i] >= s2[i] else 0.0 for i in each], False
    if vo == V.kSne:
        return [1.0 if s1[i] != s2[i] else 0.0 for i in each], False
    if vo == V.kFrc:
        return [s1[i] - _floor(s1[i]) for i in each], False
    if vo == V.kTrunc:
        return [_trunc(s1[i]) for i in each], False
    if vo == V.kFloor:
        return [_floor(s1[i]) for i in each], False
    if vo == V.kMad:                             # fused: one rounding, after the add
        return [s1[i] * s2[i] + s3[i] for i in each], False
    if vo == V.kCndEq:
        return [s2[i] if s1[i] == 0.0 else s3[i] for i in each], False
    if vo == V.kCndGe:
        return [s2[i] if s1[i] >= 0.0 else s3[i] for i in each], False
    if vo == V.kCndGt:
        return [s2[i] if s1[i] > 0.0 else s3[i] for i in each], False
    if vo == V.kDp4:
        return [sum(s1[i] * s2[i] for i in each)] * 4, False
    if vo == V.kDp3:
        return [sum(s1[i] * s2[i] for i in range(3))] * 4, False
    if vo == V.kDp2Add:
        return [s1[0] * s2[0] + s1[1] * s2[1] + s3[0]] * 4, False
    if vo == V.kMax4:
        return [max(s1)] * 4, False
    if vo == V.kCube:
        return _cube(s1), False
    if vo in (V.kKillEq, V.kKillGt, V.kKillGe, V.kKillNe):
        test = {V.kKillEq: lambda a, b: a == b, V.kKillGt: lambda a, b: a > b,
                V.kKillGe: lambda a, b: a >= b, V.kKillNe: lambda a, b: a != b}[vo]
        kill = any(test(s1[i], s2[i]) for i in each)
        return [1.0 if kill else 0.0] * 4, kill
    raise NotImplementedError(f"simulate: vector op {vo.name}")


def _scalar(op, s3, ps, state):
    """A scalar op's result from src3's a (w) and b (x), the previous scalar result ps; sets a0, p0 and kills in
    state."""
    so = op.scalar_opcode()
    a, b = s3[3], s3[0]
    if so == S.kAdds:
        return a + b
    if so == S.kAddsPrev:
        return a + ps
    if so == S.kMuls:
        return a * b
    if so == S.kMulsPrev:
        return a * ps
    if so == S.kMaxs:
        return a if a >= b else b
    if so == S.kMins:
        return a if a < b else b
    if so == S.kSeqs:
        return 1.0 if a == 0.0 else 0.0
    if so == S.kSgts:
        return 1.0 if a > 0.0 else 0.0
    if so == S.kSges:
        return 1.0 if a >= 0.0 else 0.0
    if so == S.kSnes:
        return 1.0 if a != 0.0 else 0.0
    if so == S.kFrcs:
        return a - _floor(a)
    if so == S.kTruncs:
        return _trunc(a)
    if so == S.kFloors:
        return _floor(a)
    if so == S.kExp:
        return 1.0 if a == 0.0 else (2.0 ** a if a < 1024 else math.inf)
    if so in (S.kLog, S.kLogc):
        if a == 1.0:
            return 0.0
        t = math.log2(a) if a > 0 else (-math.inf if a == 0 else math.nan)
        return -FLT_MAX if so == S.kLogc and t == -math.inf else t
    if so in (S.kRcp, S.kRcpc, S.kRcpf):
        t = 1.0 if a == 1.0 else (1.0 / a if a != 0 else math.copysign(math.inf, a))
        if so == S.kRcpc and math.isinf(t):
            t = math.copysign(FLT_MAX, t)
        if so == S.kRcpf and math.isinf(t):
            t = math.copysign(0.0, t)
        return t
    if so in (S.kRsq, S.kRsqc, S.kRsqf):
        if a == 1.0:
            t = 1.0
        elif a == 0.0:
            t = math.copysign(math.inf, a)
        elif a < 0.0:
            t = math.nan
        else:
            t = 1.0 / math.sqrt(a)
        if so == S.kRsqc and math.isinf(t):
            t = math.copysign(FLT_MAX, t)
        if so == S.kRsqf and math.isinf(t):
            t = math.copysign(0.0, t)
        return t
    if so in (S.kMaxAs, S.kMaxAsf):
        index = math.floor(a + 0.5) if so == S.kMaxAs else math.floor(a)
        state["a0"] = int(max(-256.0, min(255.0, index)))
        return a if a >= b else b
    if so in (S.kSubs, S.kSubsc0, S.kSubsc1):
        return a - b
    if so in (S.kMulsc0, S.kMulsc1):
        return a * b
    if so in (S.kAddsc0, S.kAddsc1):
        return a + b
    if so == S.kSubsPrev:
        return a - ps
    if so in (S.kSetpEq, S.kSetpNe, S.kSetpGt, S.kSetpGe):
        test = {S.kSetpEq: a == 0.0, S.kSetpNe: a != 0.0, S.kSetpGt: a > 0.0, S.kSetpGe: a >= 0.0}[so]
        state["p0"] = test
        return 0.0 if test else 1.0
    if so in (S.kKillsEq, S.kKillsGt, S.kKillsGe, S.kKillsNe, S.kKillsOne):
        kill = {S.kKillsEq: a == 0.0, S.kKillsGt: a > 0.0, S.kKillsGe: a >= 0.0, S.kKillsNe: a != 0.0,
                S.kKillsOne: a == 1.0}[so]
        state["killed"] = state["killed"] or kill
        return 1.0 if kill else 0.0
    if so == S.kSqrt:
        return math.sqrt(a) if a >= 0 else math.nan
    if so == S.kSin:
        return math.sin(a)
    if so == S.kCos:
        return math.cos(a)
    if so == S.kRetainPrev:
        return ps
    raise NotImplementedError(f"simulate: scalar op {so.name}")


def _coordinates(fetch, regs):
    """The 3 coordinates a texture fetch's source swizzle picks."""
    src = regs.get(fetch.src(), [0.0] * 4)
    return [src[(fetch.src_swizzle() >> (2 * i)) & 3] for i in range(3)]


def _quad_gradients(rec, lanes):
    """Gives each lane the texture fetch's coordinates' coarse gradients over its quad, lanes 4q to 4q + 3 its top left,
    top right, bottom left and bottom right: ddx from the top left to the top right, ddy to the bottom left; and the
    gradients the top left's setters set."""
    coords = [_coordinates(xu.TextureFetchInstruction(*rec.raw), regs) for regs, _, _ in lanes]
    for q in range(0, len(lanes), 4):
        ddx = [single(coords[q + 1][i] - coords[q][i]) for i in range(3)]
        ddy = [single(coords[q + 2][i] - coords[q][i]) for i in range(3)]
        top_left = [list(g) for g in lanes[q][2]["gradients"]]
        for _, _, state in lanes[q:q + 4]:
            state["quad"] = (ddx, ddy)
            state["quad_setters"] = top_left


def _execute(rec, regs, consts, exports, state, vertex_fetch, texture_fetch, texture_lod):
    """One instruction of an exec that runs."""
    if rec.kind == "vfetch":
        fetch = xu.VertexFetchInstruction(*rec.raw)
        if fetch.is_mini_fetch() or vertex_fetch is None:
            raise NotImplementedError("simulate: a vertex fetch with no vertex_fetch, or a mini fetch")
        index = regs.get(fetch.src(), [0.0] * 4)[fetch.src_swizzle()]
        value = [single(x) for x in vertex_fetch(state["fetches"], fetch, index)]
        state["fetches"] += 1
        _write_fetch(regs, fetch.dest(), fetch.dest_swizzle(), value)
        return
    if rec.kind == "tfetch":
        fetch = xu.TextureFetchInstruction(*rec.raw)
        coords = _coordinates(fetch, regs)
        if fetch.opcode() == SET_LOD:
            state["lod"] = coords[0]
            return
        if fetch.opcode() in SET_GRADIENTS:
            state["gradients"][SET_GRADIENTS.index(fetch.opcode())] = coords
            return
        quad = state.get("quad")
        if fetch.opcode() == GET_GRADIENTS and quad:
            # ddx and ddy of the first coordinate, then of the second (spirv_shader_translator_fetch.cc:621-633).
            _write_fetch(regs, fetch.dest(), fetch.dest_swizzle(), [quad[0][0], quad[1][0], quad[0][1], quad[1][1]])
            return
        if texture_lod is not None:
            quad = quad if fetch.use_computed_lod() else None
            setters = state.get("quad_setters", state["gradients"])
            gradients = setters if fetch.use_register_gradients() else quad
            lod = Lod(fetch.use_computed_lod(), fetch.lod_bias(),
                      state["lod"] if fetch.use_register_lod() else None,
                      tuple(list(g) for g in gradients) if gradients else None)
            value = texture_lod(fetch.fetch_constant_index(), coords, lod)
        elif texture_fetch is not None:
            value = texture_fetch(fetch.fetch_constant_index(), coords)
        else:
            raise NotImplementedError("simulate: a texture fetch with no texture_fetch")
        _write_fetch(regs, fetch.dest(), fetch.dest_swizzle(), [single(x) for x in value])
        return
    if rec.kind != "alu":
        return
    op = xu.AluInstruction(*rec.raw)
    if op.is_predicated() and op.predicate_condition() != state["p0"]:
        return
    s = {i: _source(op, i, regs, consts, state) for i in (1, 2, 3)}
    if op.scalar_with_constant():
        s[3] = _with_constant(op, regs, consts, state)
    vector, kill = _vector(op, s[1], s[2], s[3])
    state["killed"] = state["killed"] or kill
    scalar = _scalar(op, s[3], state["ps"], state)
    if op.vector_clamp():
        vector = [_saturate(x) for x in vector]
    if op.scalar_clamp():
        scalar = _saturate(scalar)
    vector = [single(x) for x in vector]
    scalar = single(scalar)
    if op.is_export():
        out = exports.setdefault(op.vector_dest(), [0.0] * 4)
        vmask, smask = op.GetVectorOpResultWriteMask(), op.GetScalarOpResultWriteMask()
        for i in range(4):
            if vmask & (1 << i):
                out[i] = vector[i]
            if smask & (1 << i):
                out[i] = scalar
            if op.GetConstant1WriteMask() & (1 << i):
                out[i] = 1.0
            if op.GetConstant0WriteMask() & (1 << i):
                out[i] = 0.0
    else:
        vmask = op.GetVectorOpResultWriteMask()
        if vmask:
            out = regs.setdefault(op.vector_dest(), [0.0] * 4)
            for i in range(4):
                if vmask & (1 << i):
                    out[i] = vector[i]
        smask = op.GetScalarOpResultWriteMask()
        if smask:
            out = regs.setdefault(op.scalar_dest(), [0.0] * 4)
            for i in range(4):
                if smask & (1 << i):
                    out[i] = scalar
    state["ps"] = scalar


# More control flow steps than any program xsc writes takes, so a loop that never ends is caught.
MAX_STEPS = 1 << 20
# LOOP_START nests 4 deep (xenia:src/xenia/gpu/ucode.h:131).
MAX_LOOP_DEPTH = 4


def _control_flow(blob):
    """Each control flow instruction's record by its index, and an exec's instructions."""
    c = container.Container(blob)
    cf, body, current = {}, {}, None
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind == "cf":
            cf[rec.offset] = rec
            current = rec.offset
            body[current] = []
        elif current is not None:
            body[current].append(rec)
    return cf, body


def run(blob: bytes, registers: dict | None = None, constants: dict | None = None, vertex_fetch=None,
        texture_fetch=None, loops: dict | None = None, texture_lod=None) -> Result:
    """Runs the program in the container blob.

    registers: temp -> 4 floats at the start (a pixel shader's interpolators and pixel parameters; a vertex shader's
    vertex index in r0). constants: float constant -> 4 floats; others read 0. vertex_fetch(n, fetch, index) gives
    the 4 floats the program's nth vertex fetch reads for the vertex index it reads from its source register;
    texture_fetch(slot, coordinates) the 4 a texture fetch from fetch constant slot samples at the 3 coordinates its
    source swizzle picks; texture_lod(slot, coordinates, lod), given in its place, the same with the fetch's Lod.
    loops: loop constant -> its count of passes; without it each loop's body runs once.
    """
    return run_group(blob, [registers or {}], constants, vertex_fetch, texture_fetch, loops, texture_lod)[0]


def run_group(blob: bytes, registers: list, constants: dict | None = None, vertex_fetch=None, texture_fetch=None,
              loops: dict | None = None, texture_lod=None, quads: bool = False) -> list:
    """Runs the program for a group of vertices or pixels in lockstep, each one's registers at the start in the list
    registers, the rest as run gives them; a Result for each, in order. With quads, each four lanes are a 2x2 quad of
    pixels, top left, top right, bottom left and bottom right, whose getGradients and computed LODs read each other,
    and whose fetches at register gradients all take the top left's."""
    if quads and len(registers) % 4:
        raise ValueError("simulate: quads of a group that isn't whole quads")
    consts = constants or {}
    lanes = [({r: [single(x) for x in v] for r, v in each.items()}, {},
              {"a0": 0, "aL": 0, "p0": False, "killed": False, "ps": 0.0, "fetches": 0, "lod": 0.0,
               "gradients": [[0.0] * 3, [0.0] * 3]}) for each in registers]
    cf, body = _control_flow(blob)
    pc, stack, steps, passes = 0, [], 0, 0
    while pc in cf:
        steps += 1
        if steps > MAX_STEPS:
            raise RuntimeError("simulate: the program's control flow never ends")
        d = cf[pc].detail
        following = pc + 1
        if isinstance(d, xu.ParsedExecInstruction):
            if d.type == xu._ExecType.kConditional:
                raise NotImplementedError("simulate: an exec on a boolean constant")
            on = [d.type == xu._ExecType.kUnconditional or d.condition == s["p0"] for _, _, s in lanes]
            runs = any(on)
            for rec in body[pc] if runs else ():
                if quads and rec.kind == "tfetch":
                    _quad_gradients(rec, lanes)
                for (regs, exports, state), lane_on in zip(lanes, on):
                    if lane_on:
                        _execute(rec, regs, consts, exports, state, vertex_fetch, texture_fetch, texture_lod)
            if d.is_end and runs:
                break
        elif isinstance(d, xu.ParsedLoopStartInstruction):
            count = 1 if loops is None else loops.get(d.loop_constant_index, 0)
            if count <= 0:
                following = d.loop_skip_address
            elif len(stack) == MAX_LOOP_DEPTH:
                raise RuntimeError("simulate: a fifth nested loop")
            else:
                stack.append([0, count])
                for _, _, state in lanes:
                    state["aL"] = 0
        elif isinstance(d, xu.ParsedAllocInstruction):
            # a0 is lost at an alloc on the console, where Xenia keeps it (xsdk:docs/HARDWARE.md, "a0 doesn't survive
            # an alloc").
            for _, _, state in lanes:
                state["a0"] = None
        elif isinstance(d, xu.ParsedLoopEndInstruction):
            passes += 1
            stack[-1][0] += 1
            done, count = stack[-1]
            breaks = d.is_predicated_break and all(d.predicate_condition == s["p0"] for _, _, s in lanes)
            if done < count and not breaks:
                following, al = d.loop_body_address, done
            else:
                stack.pop()
                al = stack[-1][0] if stack else 0
            for _, _, state in lanes:
                state["aL"] = al
        pc = following
    return [Result(exports, state["killed"], regs, passes) for regs, exports, state in lanes]


def _address(op, state):
    """The register a constant read at an index is past: a0's, or aL's."""
    if not op.is_const_address_register_relative():
        return state["aL"]
    if state["a0"] is None:
        raise RuntimeError("simulate: a constant read at a0 past an alloc with no a0 load since")
    return state["a0"]


def _source(op, i, regs, consts, state):
    reg = op.src_reg(i)
    if op.src_is_temp(i):
        value = list(regs.get(reg & 0x3F, [0.0] * 4))
        if reg & 0x80:
            value = [abs(x) for x in value]
    else:
        if op.src_const_is_addressed(i):
            reg += _address(op, state)
        value = list(consts.get(reg, [0.0] * 4))
        if op.abs_constants():
            value = [abs(x) for x in value]
    value = [value[xu.AluInstruction.GetSwizzledComponentIndex(op.src_swizzle(i), k)] for k in range(4)]
    return [-x for x in value] if op.src_negate(i) else value


def _with_constant(op, regs, consts, state):
    """mulsc's, addsc's and subsc's a, a constant's w, in lane w and b, a temp's x, in lane x, one negation for both
    (xenia:src/xenia/gpu/shader_interpreter.cc:655-670). Where the constant's component is the temp's, the console
    reads the temp for a too, as probe_scalar_forms showed (xsdk:docs/HARDWARE.md, "The scalar unit's constant forms,
    milestone 48 step 5"); Xenia doesn't."""
    reg = op.src_reg(3)
    if op.src_const_is_addressed(3):
        reg += _address(op, state)
    lane = xu.AluInstruction.GetSwizzledComponentIndex
    b = regs.get(op.scalar_const_reg_op_src_temp_reg(), [0.0] * 4)[lane(op.src_swizzle(3), 0)]
    a = b if op.scalar_constant_reads_temp() else consts.get(reg, [0.0] * 4)[lane(op.src_swizzle(3), 3)]
    a, b = (-a, -b) if op.src_negate(3) else (a, b)
    return [b, 0.0, 0.0, a]


def _write_fetch(regs, dest, swizzle, value):
    out = regs.setdefault(dest, [0.0] * 4)
    for i in range(4):
        sel = (swizzle >> (3 * i)) & 7
        if sel < 4:
            out[i] = value[sel]
        elif sel == 4:
            out[i] = 0.0
        elif sel == 5:
            out[i] = 1.0
