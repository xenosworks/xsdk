#!/usr/bin/env python3
"""needless.py: counts the work in a compiled shader that its arithmetic doesn't need, the kinds milestone 48 goes
after (docs/scope/48-xost-code-quality-2.md), on the microcode as xdis disassembles it. tools/check_optimize.py prints
and saves the counts; results/2026-10-19-milestone-48/README.md defines each.

A walk of the program gives each register component a value: the number itself where the compiler knows it (a
literal, a fetch's 0 or 1, or an operation on those alone), else a range it lies in and whether it may be NaN, from
what each operation does as Xenia's interpreter runs it (xenia:src/xenia/gpu/shader_interpreter.cc:287-957; a
saturate gives 0 for NaN, xenia:src/xenia/base/math.h:64-79). An input, a uniform, a constant at a run-time index and
a fetched value may be anything. A program with flow control, predication or a register at a run-time index is
walked with every register it reads unknown.
"""
import collections
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import copy_kinds  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.equivalence as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402

V, S = xu.AluVectorOpcode, xu.AluScalarOpcode
INF = math.inf
COUNTS = ("identities", "mix_pairs", "divided_lanes", "constant_rcps", "single_lane", "range_useless", "without_flow")
RCPS = {S.kRcp, S.kRcpc, S.kRcpf}
COMPUTES = copy_kinds.SCALAR_TWO | copy_kinds.SCALAR_ONE | copy_kinds.SCALAR_PREVIOUS | {S.kRetainPrev}
COMPARES = {V.kSeq, V.kSgt, V.kSge, V.kSne}


def f32(x):
    """x as the GPU's float holds it; past the largest, infinite."""
    if not math.isfinite(x):
        return x
    if abs(x) > simulate.FLT_MAX:
        return math.copysign(INF, x)
    return struct.unpack("f", struct.pack("f", x))[0]


class Value:
    """A component's value: known, the number where the compiler knows it; else lo to hi, and nan if it may be NaN.
    writer is the (step, pipe) that wrote it, lane the lane it was computed in, and source the value a copy copies,
    itself otherwise."""

    def __init__(self, lo=-INF, hi=INF, nan=True, known=None, writer=None, lane=0):
        if known is not None:
            known = f32(known)
            lo, hi, nan = (-INF, INF, True) if math.isnan(known) else (known, known, False)
        self.lo, self.hi, self.nan, self.known = f32(lo), f32(hi), nan, known
        self.writer, self.lane, self.source = writer, lane, self

    def fixed(self):
        """The number, known or one the range leaves."""
        if self.known is not None:
            return self.known
        return self.lo if self.lo == self.hi and not self.nan else None

    def within(self, lo, hi):
        return not self.nan and self.lo >= lo and self.hi <= hi


def saturate(lo, hi, nan):
    return min(max(lo, 0.0), 1.0) if not nan else 0.0, min(max(hi, 0.0), 1.0), False


def add(a, b):
    lo, hi = a.lo + b.lo, a.hi + b.hi
    nan = a.nan or b.nan or (a.hi == INF and b.lo == -INF) or (a.lo == -INF and b.hi == INF)
    return (-INF if math.isnan(lo) else lo), (INF if math.isnan(hi) else hi), nan


def mul(a, b):
    # 0 times anything is 0 (shader_interpreter.cc:339-347), so the corners bound it with infinities too.
    ends = [simulate._mul(x, y) for x in (a.lo, a.hi) for y in (b.lo, b.hi)]
    return min(ends), max(ends), a.nan or b.nan


def maximum(a, b):
    # x if x >= y else y: a NaN x gives y, a NaN y gives y.
    lo = min(b.lo, max(a.lo, b.lo)) if a.nan else max(a.lo, b.lo)
    return lo, max(a.hi, b.hi), b.nan


def minimum(a, b):
    # x if x < y else y.
    hi = max(b.hi, min(a.hi, b.hi)) if a.nan else min(a.hi, b.hi)
    return min(a.lo, b.lo), hi, b.nan


def fraction(a):
    if math.isfinite(a.lo) and math.isfinite(a.hi) and math.floor(a.lo) == math.floor(a.hi):
        return a.lo - math.floor(a.lo), a.hi - math.floor(a.lo), a.nan
    return 0.0, 1.0, a.nan or not (math.isfinite(a.lo) and math.isfinite(a.hi))


def dot(xs, ys, extra=None):
    terms = [mul(x, y) for x, y in zip(xs, ys)] + ([(extra.lo, extra.hi, extra.nan)] if extra else [])
    lo, hi, nan = terms[0]
    for term in terms[1:]:
        lo, hi, nan = add(Value(lo, hi, nan), Value(*term))
    return lo, hi, nan


def unknown(*_):
    return -INF, INF, True


RANGES = {
    V.kAdd: lambda x, y, z: add(x, y),
    V.kMul: lambda x, y, z: mul(x, y),
    V.kMax: lambda x, y, z: maximum(x, y),
    V.kMin: lambda x, y, z: minimum(x, y),
    V.kFrc: lambda x, y, z: fraction(x),
    V.kTrunc: lambda x, y, z: (simulate._trunc(x.lo), simulate._trunc(x.hi), x.nan),
    V.kFloor: lambda x, y, z: (simulate._floor(x.lo), simulate._floor(x.hi), x.nan),
    V.kMad: lambda x, y, z: add(Value(*mul(x, y)), z),
    V.kCndEq: lambda x, y, z: (min(y.lo, z.lo), max(y.hi, z.hi), y.nan or z.nan),
    **dict.fromkeys((V.kCndGe, V.kCndGt), lambda x, y, z: (min(y.lo, z.lo), max(y.hi, z.hi), y.nan or z.nan)),
    **dict.fromkeys(COMPARES, lambda x, y, z: (0.0, 1.0, False)),
}


def scalar_range(sop, a, b):
    if sop in (S.kAdds, S.kSubs):
        return add(a, b if sop == S.kAdds else Value(-b.hi, -b.lo, b.nan))
    if sop == S.kMuls:
        return mul(a, b)
    if sop == S.kMaxs:
        return maximum(a, b)
    if sop == S.kMins:
        return minimum(a, b)
    if S.kSeqs <= sop <= S.kSnes:
        return 0.0, 1.0, False
    if sop == S.kFrcs:
        return fraction(a)
    if sop in (S.kFloors, S.kTruncs):
        f = simulate._floor if sop == S.kFloors else simulate._trunc
        return f(a.lo), f(a.hi), a.nan
    if sop == S.kExp:
        return simulate._exp2(a.lo), simulate._exp2(a.hi), a.nan
    if sop in (S.kLog, S.kLogc) and a.lo > 0.0:
        return math.log2(a.lo), simulate._log2(a.hi), a.nan
    if sop in RCPS and (a.lo > 0.0 or a.hi < 0.0):
        return simulate._recip(a.hi), simulate._recip(a.lo), a.nan
    if sop == S.kRsq and a.lo > 0.0:
        return simulate._recip(simulate._sqrt(a.hi)), simulate._recip(math.sqrt(a.lo)), a.nan
    if sop == S.kSqrt and a.lo >= 0.0:
        return math.sqrt(a.lo), simulate._sqrt(a.hi), a.nan
    if sop in (S.kSin, S.kCos):
        return -1.0, 1.0, a.nan or not (math.isfinite(a.lo) and math.isfinite(a.hi))
    return unknown()


class Read:
    """An operand's lane: the value in the register, its modifiers, and what the operation takes."""

    def __init__(self, raw, negate, absolute):
        self.raw, self.negate, self.absolute = raw, negate, absolute
        if not (negate or absolute):
            self.value = raw
            return
        lo, hi, known = raw.lo, raw.hi, raw.known
        if absolute:
            known = None if known is None else abs(known)
            lo, hi = (lo, hi) if lo >= 0.0 else (-hi, -lo) if hi <= 0.0 else (0.0, max(-lo, hi))
        if negate:
            known = None if known is None else -known
            lo, hi = -hi, -lo
        self.value = Value(lo, hi, raw.nan, known)

    def same(self, other):
        return self.raw.source is other.raw.source and self.absolute == other.absolute


class Step:
    """An ALU or fetch instruction as the walk saw it: reads[pipe] is [(operand, lane, Read)], result the vector
    op's Value in each lane it writes, scalar the scalar op's Value."""

    def __init__(self, rec, index):
        self.rec, self.index = rec, index
        self.alu = xu.AluInstruction(*rec.raw) if rec.kind == "alu" else None
        self.reads = collections.defaultdict(list)
        self.result, self.scalar = {}, None

    def lanes(self):
        mask = self.alu.GetVectorOpResultWriteMask()
        return [lane for lane in range(4) if mask >> lane & 1]

    def operand(self, n, lane):
        return next(r for m, ln, r in self.reads["vector"] if m == n and ln == lane)

    def moves(self):
        """The vector op is a max of one source with itself: a copy, clamped or not."""
        d = self.rec.detail
        return self.alu.vector_opcode() == V.kMax and d.vector_operands[0] == d.vector_operands[1]

    def is_vector(self):
        return self.alu is not None and bool(self.alu.GetVectorOpResultWriteMask()) and \
            self.alu.vector_opcode() not in copy_kinds.STATE


def straight_line(records):
    """Whether the program runs every instruction once, in order: no flow control past exec and alloc, no
    predication, no register at a run-time index."""
    for rec in records:
        if rec.kind == "cf":
            if not (rec.text.split() or [""])[0].startswith(("exec", "alloc", "cnop")):
                return False
        elif rec.kind == "alu":
            a = xu.AluInstruction(*rec.raw)
            temps = [a.src_reg(n) for n in (1, 2, 3) if a.src_is_temp(n)]
            relative = a.is_vector_dest_relative() or (a.is_scalar_dest_relative() and not a.is_export())
            if a.is_predicated() or relative or any(reg & 0x40 for reg in temps):
                return False
        else:
            f = copy_kinds.fetch_of(rec)
            if f.is_predicated() or f.is_dest_relative() or f.is_src_relative():
                return False
    return True


class Walk:
    """The program's steps, with each value read and computed."""

    def __init__(self, records, literals):
        self.literals, self.flow = literals, straight_line(records)
        self.regs, self.constants, self.previous = {}, {}, Value()
        self.steps = []
        for rec in records:
            if rec.kind in ("alu", "tfetch", "vfetch"):
                step = Step(rec, len(self.steps))
                self.steps.append(step)
                (self.alu if step.alu else self.fetch)(step)

    def temp(self, reg, comp):
        if not self.flow:
            return Value()
        return self.regs.setdefault((reg, comp), Value())

    def read(self, a, n, lane):
        comp = copy_kinds.component(a.src_swizzle(n), lane)
        if a.src_is_temp(n):
            reg = a.src_reg(n)
            return Read(self.temp(reg & 0x3F, comp), a.src_negate(n), bool(reg & 0x80))
        # abs_constants takes a constant's absolute value (xenia:src/xenia/gpu/shader_translator.cc:1238).
        if a.src_const_is_addressed(n):
            return Read(Value(), a.src_negate(n), a.abs_constants())
        key = (a.src_reg(n), comp)
        if key not in self.constants:
            values = self.literals.get(key[0])
            self.constants[key] = Value(known=values[comp]) if values else Value()
        return Read(self.constants[key], a.src_negate(n), a.abs_constants())

    def fetch(self, step):
        f = step.fetch = copy_kinds.fetch_of(step.rec)
        step.reads["fetch"] = [(1, c, Read(self.temp(f.src(), c), False, False)) for c in range(4)]
        for c in range(4):
            pick = xu.GetFetchDestinationComponentSwizzle(f.dest_swizzle(), c)
            if pick != 7:
                # A fetch's 5 is 1 and its 4 and 6 are 0 (xdis/equivalence.py, _store_fetch).
                known = None if pick < 4 else 1.0 if pick == 5 else 0.0
                self.regs[(f.dest(), c)] = Value(known=known, writer=(step.index, "fetch"), lane=c)

    def alu(self, step):
        a = step.alu
        vop, sop = a.vector_opcode(), a.scalar_opcode()
        vector = a.GetVectorOpResultWriteMask() or vop in copy_kinds.STATE
        if vector:
            table = copy_kinds.VECTOR_READS.get(vop, (copy_kinds.ALL,) * 3)
            for n, lanes in enumerate(table, 1):
                lanes = a.GetVectorOpResultWriteMask() if lanes is None else lanes
                step.reads["vector"] += [(n, lane, self.read(a, n, lane)) for lane in range(4) if lanes >> lane & 1]
        scalar = sop != S.kRetainPrev or a.GetScalarOpResultWriteMask()
        if scalar:
            lanes = 0 if sop == S.kRetainPrev else 0x9 if sop in copy_kinds.SCALAR_TWO else 0x8 if sop in COMPUTES \
                else copy_kinds.ALL
            step.reads["scalar"] = [(3, lane, self.read(a, 3, lane)) for lane in range(4) if lanes >> lane & 1]
            if sop in copy_kinds.SCALAR_PREVIOUS or sop == S.kRetainPrev:
                step.reads["scalar"].append((0, 0, Read(self.previous, False, False)))
        # Both pipes read before either writes (shader_interpreter.cc:287-955).
        if vector:
            step.result = self.vector_values(step)
        if scalar:
            step.scalar = self.previous = self.scalar_value(step)
        if a.is_export():
            return
        for lane, v in step.result.items():
            self.regs[(a.vector_dest(), lane)] = v
        for lane in range(4):
            if a.GetScalarOpResultWriteMask() >> lane & 1:
                self.regs[(a.scalar_dest(), lane)] = step.scalar

    def vector_values(self, step):
        """The vector op's Value in each lane it writes."""
        a, op = step.alu, step.alu.vector_opcode()
        ops = collections.defaultdict(lambda: [None] * 4)
        for n, lane, r in step.reads["vector"]:
            ops[n][lane] = r.value
        ops = [ops[n] for n in (1, 2, 3)]
        try:
            knowns = simulate._vector(op, [[None if v is None else v.known for v in vs] for vs in ops])
        except simulate.Unsupported:
            knowns = [None] * 4
        values = {}
        for lane in step.lanes():
            known = knowns[lane]
            if known is not None:
                lo, hi, nan = known, known, math.isnan(known)
            elif op in RANGES:
                lo, hi, nan = RANGES[op](*(vs[lane] or Value() for vs in ops))
            elif op in (V.kDp4, V.kDp3, V.kDp2Add):
                n = 4 if op == V.kDp4 else 3 if op == V.kDp3 else 2
                lo, hi, nan = dot(ops[0][:n], ops[1][:n], ops[2][0] if op == V.kDp2Add else None)
            elif op == V.kMax4:
                nan = any(v.nan for v in ops[0])
                lo, hi = (min if nan else max)(v.lo for v in ops[0]), max(v.hi for v in ops[0])
            else:
                lo, hi, nan = unknown()
            if a.vector_clamp():
                lo, hi, nan = saturate(lo, hi, nan)
                known = None if known is None else 0.0 if math.isnan(known) else min(max(known, 0.0), 1.0)
            values[lane] = Value(lo, hi, nan, known, (step.index, "vector"), lane)
            first = step.operand(1, lane) if step.moves() else None
            if first and not (first.negate or first.absolute or a.vector_clamp()):
                values[lane].source = first.raw.source
        return values

    def scalar_value(self, step):
        a, sop = step.alu, step.alu.scalar_opcode()
        if sop == S.kRetainPrev:
            v = self.previous
            lo, hi, nan, known = v.lo, v.hi, v.nan, v.known
        else:
            src = {lane: r.value for n, lane, r in step.reads["scalar"] if n == 3}
            x, y = src.get(3, Value()), src.get(0, Value())
            f = simulate._SCALAR.get(sop)
            two = sop in copy_kinds.SCALAR_TWO
            known = None
            if f and sop in COMPUTES and x.known is not None and (not two or y.known is not None):
                known = f(x.known, y.known if two else 0.0)
            lo, hi, nan = scalar_range(sop, x, y) if sop in COMPUTES else unknown()
        if a.scalar_clamp():
            lo, hi, nan = saturate(lo, hi, nan)
            known = None if known is None else 0.0 if math.isnan(known) else min(max(known, 0.0), 1.0)
        return Value(lo, hi, nan, known, (step.index, "scalar"))


def every(step, n, test, number):
    """Whether operand n has a number passing test in every lane the vector op writes."""
    return all((x := number(step.operand(n, lane).value)) is not None and test(x) for lane in step.lanes())


def folds(step, number):
    """What an add, mul or mad folds to with number giving an operand lane's number, or an op on numbers alone:
    (kind, kept), kept the operand left as the result, 0 for a constant, None for a cheaper operation; or None."""
    op = step.alu.vector_opcode()
    if not step.moves() and all(number(r.value) is not None for _, _, r in step.reads["vector"]):
        return "on constants alone", 0
    zero, one, minus_one = (lambda x: x == 0.0), (lambda x: x == 1.0), (lambda x: x == -1.0)
    for n, other in ((2, 1), (1, 2)):
        if op == V.kAdd and every(step, n, zero, number):
            return ("0 - x, a negation" if step.alu.src_negate(other) else "x + 0"), other
        if op == V.kMul and every(step, n, zero, number):
            return "x * 0", 0
        if op == V.kMul and every(step, n, one, number):
            return "x * 1", other
        if op == V.kMul and every(step, n, minus_one, number):
            return "x * -1, a negation", other
        if op == V.kMad and every(step, n, zero, number):
            return "mad by 0", 3
    if op != V.kMad:
        return None
    by_one = [other for n, other in ((2, 1), (1, 2)) if every(step, n, one, number)]
    if every(step, 3, zero, number):
        return ("mad by 1 adding 0", by_one[0]) if by_one else ("mad adding 0, a mul", None)
    if by_one or any(every(step, n, minus_one, number) for n in (1, 2)):
        return "mad by 1, an add", None
    return None


def keeps(op, k, x, y):
    """Whether a max or min of x and y always gives operand k's value."""
    if op == V.kMax:
        return not x.nan and not y.nan and x.lo >= y.hi if k == 1 else x.hi <= y.lo
    return not x.nan and not y.nan and x.hi <= y.lo if k == 1 else x.lo >= y.hi


def unchanged(step):
    """(kind, kept) for a min, max or clamp the ranges of what it reads say can't change its value, or None."""
    a, op, lanes = step.alu, step.alu.vector_opcode(), step.lanes()
    if step.moves():
        if a.vector_clamp() and all(step.operand(1, lane).value.within(0.0, 1.0) for lane in lanes):
            return "a clamp of a value in 0 to 1", 1
        return None
    if op not in (V.kMax, V.kMin):
        return None
    for k in (1, 2):
        values = [step.operand(k, lane).value for lane in lanes]
        if all(keeps(op, k, step.operand(1, lane).value, step.operand(2, lane).value) for lane in lanes) and \
                (not a.vector_clamp() or all(v.within(0.0, 1.0) for v in values)):
            return ("a max" if op == V.kMax else "a min") + " that can't change its value", k
    return None


def range_folds(step):
    """(kind, kept) for a vector op the ranges make useless: a clamp that can't change its value, a result the range
    fixes, or an identity with a number the range fixes."""
    found = unchanged(step)
    if found:
        return found
    if not step.moves() and all(v.fixed() is not None for v in step.result.values()):
        return "a value its range fixes", 0
    found = folds(step, Value.fixed)
    return ("an identity with a number its range fixes", found[1]) if found and found[1] is not None else None


def needed(walk, removed):
    """The (step, pipe)s the exports, the fetches and the ops that set state need, with removed[step] the operand a
    removed vector op leaves, or 0 for none."""
    def needs(step, pipe):
        kept = removed.get(step.index) if pipe == "vector" else None
        reads = [r for n, _, r in step.reads[pipe] if kept is None or n == kept]
        return {r.raw.writer for r in reads if r.raw.writer}
    roots = set()
    for step in walk.steps:
        if step.alu is None:
            roots.add((step.index, "fetch"))
            continue
        export = step.alu.is_export()
        if step.reads["vector"] and (export or step.alu.vector_opcode() in copy_kinds.STATE):
            roots.add((step.index, "vector"))
        if step.reads["scalar"] and (export or step.alu.scalar_opcode() not in COMPUTES):
            roots.add((step.index, "scalar"))
    seen, todo = set(roots), list(roots)
    while todo:
        i, pipe = todo.pop()
        for node in needs(walk.steps[i], pipe) - seen:
            seen.add(node)
            todo.append(node)
    return seen


def divided(walk):
    """Each one-lane mul that takes its lane's quotient from an rcp: (step, the rcp's input as read)."""
    out = []
    for step in walk.steps:
        if not step.is_vector() or step.alu.vector_opcode() != V.kMul or len(step.lanes()) != 1:
            continue
        lane = step.lanes()[0]
        for n in (1, 2):
            w = step.operand(n, lane).raw.source.writer
            if w and w[1] == "scalar" and walk.steps[w[0]].alu.scalar_opcode() in RCPS:
                out.append((step, next(r for m, ln, r in walk.steps[w[0]].reads["scalar"] if m == 3 and ln == 3)))
                break
    return out


def mix_pair(walk, step):
    """Which of a mix's a, x and y is a known 0 or 1, for a mad x + (y - x) * a whose y - x an add computes, as
    xsdk:src/xsc_lower_glsl.c's mix lowers it; or None."""
    if step.alu.vector_opcode() != V.kMad:
        return None
    for t, n in ((1, 2), (2, 1)):
        x, y, a = [], [], []
        for lane in step.lanes():
            r = step.operand(t, lane)
            w = r.raw.source.writer
            if not w or w[1] != "vector" or r.negate or r.absolute or walk.steps[w[0]].alu.vector_opcode() != V.kAdd:
                break
            diff = walk.steps[w[0]]
            p, q = diff.operand(1, r.raw.source.lane), diff.operand(2, r.raw.source.lane)
            minus, plus = (q, p) if q.negate and not p.negate else (p, q) if p.negate and not q.negate else (None, None)
            third = step.operand(3, lane)
            if minus is None or third.negate or not third.same(minus):
                break
            x.append(third.value)
            y.append(plus.value)
            a.append(step.operand(n, lane).value)
        else:
            for name, values in (("a", a), ("x", x), ("y", y)):
                if all(v.known in (0.0, 1.0) for v in values):
                    return name
            return None
    return None


def count(blob, manifest):
    """The program's counts of needless work, with "kinds" counting each kind by name."""
    c = container.Container(blob)
    records = xu._decode_program(c.ucode_dwords, c.shader_type)[0]
    literals = {entry[0]: entry[1] for entry in manifest if len(entry) == 2 and isinstance(entry[0], int)}
    walk = Walk(records, literals)
    kinds = collections.Counter()
    n = dict.fromkeys(COUNTS, 0)
    n["without_flow"] = int(not walk.flow)
    literal, folded = {}, set()
    for step in walk.steps:
        if not step.is_vector():
            continue
        found = folds(step, lambda v: v.known)
        if found:
            n["identities"] += 1
            kinds["identity: " + found[0]] += 1
            folded.add(step.index)
            if found[1] is not None:
                literal[step.index] = found[1]
        pair = mix_pair(walk, step)
        if pair:
            n["mix_pairs"] += 1
            kinds[f"mix pair: {pair} 0 or 1"] += 1
        a = step.alu
        if len(step.lanes()) == 1 and not a.is_export() and not step.moves() and \
                step.rec.detail.IsScalarOpDefaultNop():
            n["single_lane"] += 1
            kinds["single-lane: " + step.rec.detail.vector_opcode_name] += 1
    quotients = divided(walk)
    sources = collections.Counter((r.raw.source, r.negate, r.absolute) for _, r in quotients if r.value.known is None)
    for step, r in quotients:
        by = "a constant" if r.value.known is not None else "one scalar" if sources[(r.raw.source, r.negate,
                                                                                         r.absolute)] > 1 else None
        if by:
            n["divided_lanes"] += 1
            kinds["divided lane: by " + by] += 1
    for step in walk.steps:
        if step.scalar and step.alu.scalar_opcode() in RCPS and step.scalar.known is not None:
            n["constant_rcps"] += 1
    if walk.flow:
        found = {s.index: range_folds(s) for s in walk.steps if s.is_vector() and s.index not in folded}
        found = {i: f for i, f in found.items() if f}
        before = needed(walk, literal)
        after = needed(walk, {**literal, **{i: f[1] for i, f in found.items()}})
        # Code already dead before isn't the ranges' doing.
        useless = [f[0] for i, f in found.items() if (i, "vector") in before]
        useless += ["code only those read"] * sum(1 for s in walk.steps if s.is_vector() and s.index not in found
                                                  and (s.index, "vector") in before - after)
        n["range_useless"] = len(useless)
        kinds.update("range: " + kind for kind in useless)
    n["kinds"] = dict(kinds)
    return n
