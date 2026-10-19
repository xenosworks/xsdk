#!/usr/bin/env python3
"""copy_kinds.py <shader or folder>...: compiles each GLSL shader, or SPIR-V file, with the compiler's optimiser at
xsdk's register split, as tools/check_optimize.py does, and sorts every copy left in the microcode, a vector max of one
source with itself and no scalar operation beside it, by what it copies and why no pass takes it
(docs/scope/45-xost-code-quality.md, "Done when"). Prints each shader's copies by kind, or with --each every copy, then
each kind's count per folder and in all. Runs in WSL, as the compiler does.

The passes ran before the allocator packed the registers, so a copy's reasons are their conditions on values
(xsdk:src/xsc_optimize.c, rewrite_reader and can_place): each read is followed through the microcode by register
component to the write it reads, and what matters is what's read with what, by whom, and after which alloc.

    python3 tools/copy_kinds.py /mnt/z/dusklight-xenon/platforms/xenon/shaders xsc/testdata/*.spv --each
"""
import argparse
import collections
import math
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import check_optimize  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402

V, S = xu.AluVectorOpcode, xu.AluScalarOpcode
ALL = 0xF
# The lanes each operand of a vector op reads (xsdk:src/xsc_optimize.c, vector_reads_table), None for an elementwise
# op's, which reads the lanes it writes.
VECTOR_READS = {**dict.fromkeys((V.kAdd, V.kMul, V.kMax, V.kMin, V.kSeq, V.kSgt, V.kSge, V.kSne), (None, None)),
                **dict.fromkeys((V.kFrc, V.kTrunc, V.kFloor), (None,)),
                **dict.fromkeys((V.kMad, V.kCndEq, V.kCndGe, V.kCndGt), (None, None, None)),
                V.kDp4: (ALL, ALL), V.kCube: (ALL, ALL), V.kMax4: (ALL,), V.kDp3: (0x7, 0x7),
                V.kDp2Add: (0x3, 0x3, 0x1), V.kDst: (0x6, 0xA)}
STATE = {V.kSetpEqPush, V.kSetpNePush, V.kSetpGtPush, V.kSetpGePush, V.kKillEq, V.kKillGt, V.kKillGe, V.kKillNe,
         V.kMaxA}
REPLICATES = {V.kDp4, V.kDp3, V.kDp2Add, V.kMax4}
SCALAR_TWO = {S.kAdds, S.kMuls, S.kMaxs, S.kMins, S.kSubs}
SCALAR_ONE = set(range(S.kSeqs, S.kRsq + 1)) | {S.kSqrt, S.kSin, S.kCos}
SCALAR_PREVIOUS = {S.kAddsPrev, S.kMulsPrev, S.kMulsPrev2, S.kSubsPrev}
KINDS = ("in a program the optimiser leaves alone", "a constant into a temp", "a constant into an export",
         "a fetched value into an export", "an input into an export", "a computed value step 2 can't move",
         "a clamp into a temp", "anything else")
ALONE, CONSTANT_TEMP, CONSTANT_EXPORT, FETCHED_EXPORT, INPUT_EXPORT, COMPUTED, CLAMP, OTHER = KINDS
INDEXED = "a constant read at a run-time index, which co_issue alone works on"
CONSTANTS = ("a literal", "a uniform", "a constant at a run-time index")
ZERO_ONE = ("the export's own 0 and 1", "a literal 0 or 1", "a vertex fetch's 0 or 1")


def elementwise(op):
    return op <= V.kMad or V.kCndEq <= op <= V.kCndGt


def component(swizzle, lane):
    return xu.AluInstruction.GetSwizzledComponentIndex(swizzle, lane)


def fetch_of(rec):
    return (xu.TextureFetchInstruction if rec.kind == "tfetch" else xu.VertexFetchInstruction)(*rec.raw)


class Step:
    """An ALU or fetch instruction: the operands each pipe names, and what it reads and writes by (register,
    component)."""

    def __init__(self, rec, stage):
        self.rec, self.stage = rec, stage
        self.alu = xu.AluInstruction(*rec.raw) if rec.kind == "alu" else None
        self.operands = {}  # pipe: operands
        self.reads = []  # (pipe, operand, lane, key)
        self.writes = {}  # key: pipe
        if self.alu is None:
            self.fetch = fetch_of(rec)
            # A fetch reads its source whole (xsdk:src/xsc_optimize.c, temp_reads).
            self.reads = [("fetch", 1, c, (self.fetch.src(), c)) for c in range(4)]
            self.writes = {(self.fetch.dest(), c): "fetch" for c in range(4)
                           if xu.GetFetchDestinationComponentSwizzle(self.fetch.dest_swizzle(), c) != 7}
            return
        a = self.alu
        if a.GetVectorOpResultWriteMask() or a.vector_opcode() in STATE:
            table = VECTOR_READS.get(a.vector_opcode(), (ALL, ALL, ALL))
            self.operands["vector"] = range(1, len(table) + 1)
            for n, lanes in enumerate(table, 1):
                self.read("vector", n, a.GetVectorOpResultWriteMask() if lanes is None else lanes)
        if a.scalar_opcode() != S.kRetainPrev:
            sop = a.scalar_opcode()
            self.operands["scalar"] = (3,)
            self.read("scalar", 3, 0x9 if sop in SCALAR_TWO else 0x8 if sop in SCALAR_ONE else ALL)
        if not a.is_export():
            self.writes.update({(a.vector_dest(), c): "vector" for c in range(4) if a.vector_write_mask() >> c & 1})
            self.writes.update({(a.scalar_dest(), c): "scalar" for c in range(4) if a.scalar_write_mask() >> c & 1})

    def read(self, pipe, n, lanes):
        if self.alu.src_is_temp(n):
            reg = self.alu.src_reg(n) & 0x3F
            self.reads += [(pipe, n, lane, (reg, component(self.alu.src_swizzle(n), lane)))
                           for lane in range(4) if lanes >> lane & 1]

    def fixed(self, pipe):
        """A pipe the passes move nothing into or out of (xsdk:src/xsc_optimize.c, fixed)."""
        if pipe == "fetch":
            return True
        if pipe == "vector":
            return self.alu.vector_opcode() in STATE
        return self.alu.scalar_opcode() not in SCALAR_TWO | SCALAR_ONE

    def is_copy(self):
        """A copy as check_optimize.py counts it."""
        d = self.rec.detail
        return (self.alu is not None and not d.IsVectorOpDefaultNop() and d.IsScalarOpDefaultNop()
                and d.vector_opcode == V.kMax and d.vector_operands[0] == d.vector_operands[1])


def left_alone(records):
    """Why the optimiser leaves the program alone (xsdk:src/xsc_optimize.c, can_optimize), or None; one reading a
    constant at a run-time index goes through co_issue alone."""
    indexed = False
    for rec in records:
        if rec.kind == "cf":
            if isinstance(rec.detail, xu.ParsedLoopStartInstruction):
                return "a hardware loop"
        elif rec.kind != "alu":
            f = fetch_of(rec)
            if f.is_predicated():
                return "predication"
            if f.is_dest_relative() or f.is_src_relative():
                return "a register at a run-time index"
        else:
            a = xu.AluInstruction(*rec.raw)
            if a.is_predicated():
                return "predication"
            if a.abs_constants():
                return "absolute constant addressing"
            if a.is_vector_dest_relative() or (a.is_scalar_dest_relative() and not a.is_export()):
                return "a register at a run-time index"
            if a.scalar_opcode() in SCALAR_PREVIOUS or (a.scalar_opcode() == S.kRetainPrev
                                                       and a.GetScalarOpResultWriteMask() and not a.is_export()):
                return "the previous scalar result"
            indexed = indexed or any(a.src_const_is_addressed(n) for n in (1, 2, 3) if not a.src_is_temp(n))
    return INDEXED if indexed else None


class Program:
    """A straight-line program's steps, each read's writer, (step, pipe) or None for an input, and each writer's
    reads."""

    def __init__(self, records, literals, widths):
        self.literals, self.widths = literals, widths
        self.steps, stage = [], 0
        for rec in records:
            if rec.kind == "cf" and isinstance(rec.detail, xu.ParsedAllocInstruction):
                stage += 1
            elif rec.kind in ("alu", "tfetch", "vfetch"):
                self.steps.append(Step(rec, stage))
        last, self.writer, self.readers = {}, {}, collections.defaultdict(list)
        for i, step in enumerate(self.steps):
            # Both pipes read before either writes (xenia:src/xenia/gpu/shader_interpreter.cc:287-955).
            for pipe, n, lane, key in step.reads:
                self.writer[(i, pipe, n, lane)] = last.get(key)
                if key in last:
                    self.readers[last[key]].append((i, pipe, n, lane, key))
            for key, pipe in step.writes.items():
                last[key] = (i, pipe)
        self.exports = collections.Counter(s.alu.vector_dest() for s in self.steps if s.alu and s.alu.is_export())

    def operand_reads(self, i, pipe, n):
        """What operand n of a pipe reads: (writer, component) for each lane."""
        return {(self.writer[(i, pipe, m, lane)], key[1]) for p, m, lane, key in self.steps[i].reads
                if p == pipe and m == n}

    def kind_of(self, w):
        """What a write is: an input, a fetched value, a constant or a computed value."""
        if w is None:
            return "an input"
        if w[1] == "fetch":
            return "a fetched value"
        step = self.steps[w[0]]
        return "a constant" if step.is_copy() and not step.alu.src_is_temp(1) else "a computed value"

    def why_reader_keeps(self, j, r, pipe):
        """Why propagate_copies can't have reader r read what copy j copies (xsdk:src/xsc_optimize.c,
        rewrite_reader), or None."""
        reader, copy = self.steps[r], self.steps[j].alu
        if reader.fixed(pipe):
            return "read by a fetch" if pipe == "fetch" else "read by a kill, a setp or an a0 load"
        mine = []
        for n in reader.operands[pipe]:
            writers = {w for w, _ in self.operand_reads(r, pipe, n)}
            if (j, "vector") in writers and len(writers) > 1:
                others = sorted({self.kind_of(w) for w in writers - {(j, "vector")}})
                return "read in one operand beside " + " and ".join(others)
            if (j, "vector") in writers:
                mine.append(n)
        if copy.src_is_temp(1):
            return None
        if any(reader.alu.src_reg(n) & 0x80 for n in mine):
            return "read as an absolute value, which a constant operand can't be"
        constants = {n: reader.alu.src_reg(n) for n in reader.operands[pipe] if not reader.alu.src_is_temp(n)}
        constants.update(dict.fromkeys(mine, copy.src_reg(1)))
        if len(set(constants.values())) > 2 or (len(constants) == 3 and constants[2] != constants[3]):
            return "read where the instruction would read a third constant"
        return None

    def why_move_stays(self, j):
        """Why a copy of a constant, a fetched value or an input into a temp stays: a reader it can't be folded into."""
        reasons = [self.why_reader_keeps(j, r, pipe) for r, pipe, _, _, _ in self.readers[(j, "vector")]]
        return next((x for x in reasons if x), "no reason found")

    def why_place_fails(self, j, w, copied):
        """Why compute_in_place can't have writer w write copy j's lanes (xsdk:src/xsc_optimize.c, can_place), or
        None; copied is every (writer, component) the copy reads. A reason that holds only as can_redirect weighs each
        writer, and each copy, alone is (reason, True)."""
        i, pipe = w
        copy, writer = self.steps[j].alu, self.steps[i]
        if writer.fixed(pipe):
            return "its writer sets state", False
        if copy.is_export() and pipe == "scalar":
            return "a scalar op's result into an export", False
        others = [(r, p, n, key) for r, p, n, _, key in self.readers[w] if r != j]
        if copy.is_export() and writer.stage != self.steps[j].stage:
            return "computed before the export's alloc" + (" and read there too" if others else ""), False
        op = writer.alu.vector_opcode()
        lanes = [lane for p, n, lane, _ in self.steps[j].reads if self.writer[(j, p, n, lane)] == w]
        if pipe == "vector" and not elementwise(op) and op not in REPLICATES:
            if any(component(copy.src_swizzle(1), lane) != lane for lane in lanes):
                return "its writer's result keeps its lanes", False
        if others and copy.is_export():
            return "its writer's result read elsewhere too", False
        if others and copy.vector_clamp():
            return "its writer's result read unclamped too", False
        alone = None
        for r, p, n, key in others:
            if self.steps[r].fixed(p):
                return "its writer's result read by a fetch or an op that sets state", False
            reads = self.operand_reads(r, p, n)
            if len({x for x, _ in reads}) > 1:
                if not reads <= copied:
                    return "its writer's result read in one operand with another writer's lanes", False
                alone = alone or ("its writer's result read in one operand with the copy's other writers' lanes", True)
            elif pipe == "vector" and op not in REPLICATES and (w, key[1]) not in copied:
                if not self.same_place(j, r):
                    return "an elementwise result whose other lanes are read elsewhere", False
                alone = alone or ("an elementwise result whose other lanes other copies take into its vector", True)
        return alone

    def same_place(self, j, r):
        """Whether step r is a copy into the vector copy j writes: the same export, or a temp read with it in one
        operand."""
        a, b = self.steps[j], self.steps[r]
        if not b.is_copy() or b.alu.is_export() != a.alu.is_export() or b.alu.vector_dest() != a.alu.vector_dest():
            return False
        if a.alu.is_export():
            return True
        return any({(j, "vector"), (r, "vector")} <= {w for w, _ in self.operand_reads(i, p, n)}
                   for i, step in enumerate(self.steps) for p, n in {(p, n) for p, n, _, _ in step.reads})

    def source(self, j):
        """What copy j copies: a literal, a uniform, an input, a fetch's value or 0 or 1, or a computed value."""
        a, mask = self.steps[j].alu, self.steps[j].alu.GetVectorOpResultWriteMask()
        if not a.src_is_temp(1):
            if a.src_const_is_addressed(1):
                return "a constant at a run-time index"
            values = self.literals.get(a.src_reg(1))
            if values is None:
                return "a uniform"
            lanes = [values[component(a.src_swizzle(1), lane)] for lane in range(4) if mask >> lane & 1]
            # An export writes a 0 or 1 itself (xenia:src/xenia/gpu/ucode.h:2051-2059), but -0 isn't its 0.
            zero_one = a.is_export() and all(v in (0.0, 1.0) and math.copysign(1.0, v) > 0 for v in lanes)
            return "a literal 0 or 1" if zero_one else "a literal"
        if not mask:
            return "the export's own 0 and 1"
        writers = {self.writer[(j, "vector", 1, lane)] for lane in range(4) if mask >> lane & 1}
        kinds = sorted({self.kind_of(w) for w in writers})
        if len(kinds) > 1:
            return " and ".join(kinds) + " together"
        if kinds[0] == "a fetched value":
            fetch = self.steps[min(writers)[0]].fetch
            picks = {xu.GetFetchDestinationComponentSwizzle(fetch.dest_swizzle(), component(a.src_swizzle(1), lane))
                     for lane in range(4) if mask >> lane & 1}
            if picks <= {4, 5}:
                return "a vertex fetch's 0 or 1"
            texture = isinstance(fetch, xu.TextureFetchInstruction)
            return "a texture fetch's value" if texture else "a vertex fetch's value"
        return kinds[0]

    def computed_reason(self, j):
        """Why no writer of computed copy j can take its place, a reason that holds whatever else holds first."""
        a = self.steps[j].alu
        mask = a.GetVectorOpResultWriteMask()
        copied = {(self.writer[(j, "vector", 1, lane)], component(a.src_swizzle(1), lane))
                  for lane in range(4) if mask >> lane & 1}
        reasons = [x for x in (self.why_place_fails(j, w, copied) for w in sorted({w for w, _ in copied})) if x]
        if not reasons:
            return "no reason found"
        reason, alone = min(reasons, key=lambda x: x[1])
        return reason + (", each weighed alone" if alone else "")

    def export_kind(self, j, what):
        a = self.steps[j].alu
        reg = a.vector_dest()
        if what == "a computed value":
            return COMPUTED, "into an export: " + self.computed_reason(j)
        if what.endswith("together"):
            return OTHER, what + " into an export"
        group = FETCHED_EXPORT if "fetch" in what else INPUT_EXPORT if what == "an input" else CONSTANT_EXPORT
        written = a.GetVectorOpResultWriteMask() | a.GetConstant0WriteMask() | a.GetConstant1WriteMask()
        # Location n exports to register n (xsdk:src/xsc_resolve.c, interp_out and color_out); oPos is 62.
        if not written & ((1 << (self.widths.get(reg, 4) if reg < 16 else 4)) - 1):
            return group, what + ", only in lanes past its output's GLSL type"
        if what in ZERO_ONE:
            return group, what + (", beside another instruction exporting the register" if self.exports[reg] > 1
                                  else ", the register's only export")
        return group, what

    def kind(self, j):
        """Copy j's kind and what in it keeps the copy."""
        a = self.steps[j].alu
        what = self.source(j)
        if a.is_export():
            group, detail = self.export_kind(j, what)
        elif what == "a computed value":
            clamp = a.vector_clamp()
            group, detail = (CLAMP, "of a computed value: ") if clamp else (COMPUTED, "into a temp: ")
            detail += self.computed_reason(j)
        elif a.vector_clamp():
            group, detail = CLAMP, "of " + what
        elif what in CONSTANTS:
            group, detail = CONSTANT_TEMP, f"{what}, {self.why_move_stays(j)}"
        else:
            group, detail = OTHER, f"{what} into a temp, {self.why_move_stays(j)}"
        if a.src_is_temp(1) and a.src_reg(1) & 0x80:
            detail += ", its absolute value"
        return group, detail + (", negated" if a.src_negate(1) else "")


def copy_kinds(spv):
    """Each copy left in the shader, optimiser on: (kind, detail, clamped, its disassembly)."""
    module = spirv.Module.parse(spv)
    blob, manifest = lower_ex(module, XSDK_REGISTERS, optimize=True)
    c = container.Container(blob)
    alone = left_alone(xu._decode_program(c.ucode_dwords, c.shader_type)[0])
    if alone and alone != INDEXED:
        # The optimiser returns at once for these (xsdk_xsc_optimize), so both programs are the same.
        assert blob == lower_ex(spirv.Module.parse(spv), XSDK_REGISTERS)[0], f"left alone for {alone}, yet changed"
    return program_copies(blob, manifest, check_optimize.output_widths(module))


def program_copies(blob, manifest, widths):
    """Each copy left in a program the optimiser had, as copy_kinds gives them."""
    c = container.Container(blob)
    records = xu._decode_program(c.ucode_dwords, c.shader_type)[0]
    alone = left_alone(records)
    literals = {entry[0]: entry[1] for entry in manifest if len(entry) == 2 and isinstance(entry[0], int)}
    p = Program(records, literals, widths)
    copies = []
    for j, step in enumerate(p.steps):
        if not step.is_copy():
            continue
        text = step.rec.text.strip()
        if alone == INDEXED:
            # Straight-line still, so what it copies can be told: shown, not counted.
            text += " ({}: {})".format(*p.kind(j))
        copies.append(((ALONE, alone) if alone else p.kind(j)) + (step.alu.vector_clamp(), text))
    assert len(copies) == check_optimize.counts(blob)["copies"], "not the copies check_optimize.py counts"
    return copies


def counted(by_folder, folders):
    return ", ".join(f"[{n}] {by_folder[f]}" for n, f in enumerate(folders) if by_folder[f])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--each", action="store_true", help="print every copy with its kind")
    args = ap.parse_args()
    sources = []
    for p in map(Path, args.paths):
        sources += sorted(q for q in p.iterdir() if q.suffix in (".vert", ".frag")) if p.is_dir() else [p]
    folders = list(dict.fromkeys(str(s.parent) for s in sources))
    totals = collections.defaultdict(collections.Counter)  # (kind, detail): copies by folder
    clamps = collections.defaultdict(collections.Counter)
    with tempfile.TemporaryDirectory() as work:
        for source in sources:
            try:
                copies = copy_kinds(check_optimize.spirv_of(source, work))
            except (LowerError, ValueError) as e:
                print(f"{source.name}: not compiled: {e}")
                continue
            for kind, detail, clamped, text in copies:
                totals[(kind, detail)][str(source.parent)] += 1
                clamps[(kind, detail)][str(source.parent)] += clamped
                if args.each:
                    print(f"{source.name}: {text}: {kind}: {detail}")
            mine = collections.Counter(f"{kind}: {detail}" for kind, detail, _, _ in copies)
            if mine and not args.each:
                print(f"{source.name}: " + "; ".join(f"{k} {n}" for k, n in sorted(mine.items())))
    print()
    for n, folder in enumerate(folders):
        print(f"[{n}] {folder}")
    for kind in KINDS:
        details = sorted((d for k, d in totals if k == kind), key=lambda d: (-sum(totals[(kind, d)].values()), d))
        if not details:
            continue
        mine = sum((totals[(kind, d)] for d in details), collections.Counter())
        print(f"{sum(mine.values()):5d}  {kind}  ({counted(mine, folders)})")
        for d in details:
            clamped = sum(clamps[(kind, d)].values())
            print(f"       {sum(totals[(kind, d)].values()):5d}  {d}" + (f", {clamped} clamped" if clamped else "")
                  + f"  ({counted(totals[(kind, d)], folders)})")
    every = sum(totals.values(), collections.Counter())
    clamped = sum(clamps.values(), collections.Counter())
    print(f"{sum(every.values()):5d}  copies in all  ({counted(every, folders)})")
    print(f"{sum(clamped.values()):5d}  of them clamped  ({counted(clamped, folders)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
