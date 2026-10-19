#!/usr/bin/env python3
"""xsc serializes an instruction that would read across the ALU and the fetches, in its exec or any later one.

The console's ALU and fetches run apart, and an exec boundary doesn't make one wait for the other: an ALU instruction
that read a fetch's result unserialized read the temp before the fetch wrote it, in the fetch's exec (Aurora's
minimap, a palette lookup at a fetched index) and in a later exec when the fetch was slow (three of Dusklight's
indirect-texturing shaders). Xenia runs each instruction to its end in turn (xsdk:docs/HARDWARE.md, "Serialized
fetches" and "Slow fetches across execs").

- The rule, run over the microcode's control flow with its serialize bits as they are: an ALU instruction needs the
  bit when it reads or writes a temp a fetch wrote, and a fetch when it reads a temp any instruction wrote, along some
  path with no serialized instruction since, as a serialized instruction waits for everything before it. A loop's
  back edge and its skip are paths, and so is passing an exec that may not run: one on the predicate, or whose
  instructions are predicated. Fetches include a vertex shader's, and a fetch reading a fetch's result is serialized
  as one reading the ALU's is, unchecked on the console.
- A run of setters, setTexLOD and the gradients', and the fetch reading them: nothing after its first setter waits or
  is ALU work before that fetch, as another pixel group's setter lands in the gap a wait leaves (xsdk:docs/HARDWARE.md,
  "Loops on the console, milestone 46 step 1"). The first setter waits for its run instead.
- In every shader of xsc's testdata, optimiser off and on, no instruction the rule wants is unserialized, and the setter
  runs hold; where the control flow runs straight through, nothing else is serialized either.
- Hand-built programs: a fetch's result read in a later exec is serialized; a serialized instruction in between
  clears it; a loop carries a fetch's result from the end of its body to its start and, as it may run no times, one
  from before it to past it; an arm on the predicate that waits leaves the other arm to wait too; a fetch waiting for
  the work goes before the fetches that don't, so it alone waits.
- vk_dependent_fetch.frag has serialized ALU and fetch instructions both, and the simulator gives what the GLSL says.
- vk_lod_dependent.frag samples at coordinates worked out from another sample, with a LOD and gradients ready sooner:
  each run's first setter waits for that work, and its fetch doesn't.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from test_local_stores import UV, check, module, push_constants  # noqa: E402
from test_optimize import T, V, alu, c, export, r  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from xsc_internals import pixel_program  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

SCALE = [1.0 / 204.0, 0.0, 0.5 / 204.0, 0.5]
INDEX = 188
TEMP = re.compile(r"\br(\d+)\b")
SETTER = re.compile(r"\bset(TexLOD|GradientH|GradientV)\b")
SLOTS = {"alu": xu.AluInstruction, "tfetch": xu.TextureFetchInstruction, "vfetch": xu.VertexFetchInstruction}


def instruction(rec):
    """Whether it's a fetch, its serialize bit, the temps it writes and reads. An ALU instruction reads each temp its
    three sources name, as xsc counts them, whether or not its operations use that source."""
    # "op dest, sources" for each of the line's operations; an export's dest is no temp.
    writes, reads = set(), set()
    if rec.kind == "alu":
        alu_ins = xu.AluInstruction(*rec.raw)
        reads = {reg & 0x3F for reg in alu_ins.src_temps().values()}
    for part in rec.text.split("+"):
        m = re.search(r"\b[a-zA-Z0-9_]+\s+([^,\s]+)\s*,(.*)", part)
        if not m:
            # setTexLOD and the gradients' setters name their source alone.
            if rec.kind == "tfetch":
                reads |= {int(t) for t in TEMP.findall(part)}
            continue
        writes |= {int(t) for t in TEMP.findall(m.group(1))}
        reads |= {int(t) for t in TEMP.findall(m.group(2))}
    return rec.kind != "alu", rec.serialize, writes, reads


def flow(blob):
    """Each control flow entry by index: the entries that may run after it, whether it may not run at all, and its
    instructions, each numbered in program order."""
    c = container.Container(blob)
    nodes, number = {}, 0
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind == "cf":
            nodes[rec.offset] = {"detail": rec.detail, "ops": [], "skippable": False}
        elif rec.kind in SLOTS:
            node = nodes[rec.exec_cf_index]
            node["ops"].append((number, instruction(rec)))
            node["skippable"] |= SLOTS[rec.kind](*rec.raw).is_predicated()
            number += 1
    for i, node in nodes.items():
        d = node["detail"]
        after = [i + 1] if i + 1 in nodes else []
        if isinstance(d, xu.ParsedExecInstruction):
            node["skippable"] |= d.type != xu._ExecType.kUnconditional
            node["next"] = [] if d.is_end else after
        elif isinstance(d, xu.ParsedLoopStartInstruction):
            node["next"] = after + [d.loop_skip_address]
        elif isinstance(d, xu.ParsedLoopEndInstruction):
            node["next"] = after + [d.loop_body_address]
        else:
            node["next"] = after
    return nodes, number


def union(a, b):
    return {t: a.get(t, set()) | b.get(t, set()) for t in a.keys() | b.keys()}


def run(ops, state, wants):
    """The exec's instructions from state, each one's need of the bit or'd into wants: what's unwaited after them."""
    state = {t: set(kinds) for t, kinds in state.items()}
    for n, (fetch, serialize, writes, reads) in ops:
        if fetch:
            wants[n] |= any(t in state for t in reads)
        else:
            wants[n] |= any("fetch" in state.get(t, ()) for t in reads | writes)
        if serialize:
            state = {}
        for t in writes:
            state.setdefault(t, set()).add("fetch" if fetch else "alu")
    return state


def hazards(blob):
    """Each instruction's serialize bit and whether the rule wants it, and whether the control flow runs straight
    through: no loop, nothing that may not run."""
    nodes, count = flow(blob)
    into = {i: {} for i in nodes}
    wants = [False] * count
    changed = True
    while changed:
        changed = False
        for i, node in sorted(nodes.items()):
            out = run(node["ops"], into[i], wants)
            if node["skippable"]:
                out = union(out, into[i])
            for j in node["next"]:
                merged = union(into[j], out)
                if merged != into[j]:
                    into[j], changed = merged, True
    serialized = [op[1] for _, node in sorted(nodes.items()) for _, op in node["ops"]]
    straight = all(len(n["next"]) <= 1 and not n["skippable"] for n in nodes.values())
    return list(zip(serialized, wants)), straight


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def setter_runs(name, blob):
    """The instructions, in program order, that start a run of setters; fails if one after a run's first waits or is
    ALU work before the fetch reading the state."""
    c = container.Container(blob)
    ops = [(rec.kind != "alu", bool(SETTER.search(rec.text)), rec.serialize)
           for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0] if rec.kind in SLOTS]
    starts = set()
    for i, (_, setter, _) in enumerate(ops):
        if not setter or (i and ops[i - 1][1]):
            continue
        starts.add(i)
        for j in range(i + 1, len(ops)):
            fetch, later_setter, serialize = ops[j]
            if not fetch or serialize:
                fail(f"{name}: instruction {j} {'waits' if fetch else 'is ALU work'} after setter {i}, before its fetch")
            if not later_setter:
                break
    return starts


def check_rule(name, blob):
    """Fails if an instruction the rule wants is unserialized, if a setter run breaks, or, in straight control flow, if
    an instruction the rule doesn't want is serialized, a run's first setter aside."""
    result, straight = hazards(blob)
    starts = setter_runs(name, blob)
    for i, (serialize, wants) in enumerate(result):
        if wants and not serialize:
            fail(f"{name}: instruction {i} reads across the ALU and the fetches unserialized")
        if straight and serialize and not wants and i not in starts:
            fail(f"{name}: instruction {i} serialized where nothing reads across")
    return straight


def ops_of(blob):
    """Every instruction in program order: its exec, then (fetch, serialize, writes, reads)."""
    nodes, _ = flow(blob)
    return [(i, op) for i, node in sorted(nodes.items()) for _, op in node["ops"]]


def first_reader(blob, fetch):
    """The exec of the program's fetch numbered fetch, and the exec and serialize bit of the first instruction after it
    reading what it wrote."""
    ops = ops_of(blob)
    at = [n for n, (_, op) in enumerate(ops) if op[0]][fetch]
    written = ops[at][1][2]
    return ops[at][0], next((e, op[1]) for e, op in ops[at + 1:] if op[3] & written)


def tfetch(dest, src):
    return dict(T["tf"], dest=dest, src=src)


def filler(dest, src):
    """Work reading neither fetch: src plus a constant."""
    return alu(V.kAdd, dest, 0xF, r(src), c(0))


def build(body, exports):
    return pixel_program(2, ce.USAGE_TEXCOORD, body, exports, XSDK_REGISTERS.pixel, False)


def check_testdata():
    straight = loose = 0
    for spv in sorted(TD.glob("*.spv")):
        for optimize in (False, True):
            try:
                blob, _ = lower_ex(spirv.Module.parse(spv.read_bytes()), XSDK_REGISTERS, optimize=optimize)
            except (LowerError, ValueError):
                continue
            if check_rule(f"{spv.name}, optimiser {'on' if optimize else 'off'}", blob):
                straight += 1
            else:
                loose += 1
    print(f"  PASS {straight + loose} programs of the testdata, optimiser off and on: nothing reads across the ALU "
          f"and the fetches unserialized, and the {straight} whose control flow runs straight serialize nothing else")


def check_later_exec():
    """A fetch, then six instructions of other work, which fill its exec, then work reading the fetch."""
    body = [tfetch(2, 0), filler(3, 1)] + [filler(n, n - 1) for n in range(4, 9)] + [alu(V.kAdd, 9, 0xF, r(8), r(2))]
    blob = build(body, [export(r(9))])
    check_rule("a fetch read in a later exec", blob)
    fetch_exec, (read_exec, serialized) = first_reader(blob, 0)
    if read_exec == fetch_exec or not serialized:
        fail(f"a fetch read in a later exec: the read in exec {read_exec}, the fetch's {fetch_exec}, serialized "
             f"{serialized}; want a later exec, serialized")
    print("  PASS a fetch's result read in a later exec is serialized")


def check_cleared():
    """Two fetches; work reading the first right after them is serialized and waits for both, so a later exec's read of
    the second needs no bit."""
    body = [tfetch(2, 0), tfetch(3, 1), alu(V.kAdd, 4, 0xF, r(2), c(0))] + [filler(n, n - 1) for n in range(5, 10)] + \
        [alu(V.kAdd, 10, 0xF, r(9), r(3))]
    blob = build(body, [export(r(10))])
    check_rule("a serialized instruction in between", blob)
    _, (_, first) = first_reader(blob, 0)
    second_exec, (read_exec, second) = first_reader(blob, 1)
    if not first or second or read_exec == second_exec:
        fail(f"a serialized instruction in between: the first fetch's reader serialized {first}, the second's {second} "
             f"in exec {read_exec}, the fetch's {second_exec}; want serialized, then not, in a later exec")
    print("  PASS a serialized instruction in between waits for both fetches, so a later exec's read of the second "
          "isn't serialized")


def check_loop():
    """r5 fetched before a loop, read past it; the body's first instruction reads r2, which the fetch ending the body
    writes. Each pass but the first reads the fetch's r2, and a loop run no times leaves r5's fetch unwaited."""
    body = [tfetch(5, 1), alu(V.kMax, 2, 0xF, r(0), r(0)), {"loop": 0}, alu(V.kAdd, 3, 0xF, r(2), c(0)),
            tfetch(2, 3), {"end_loop": True}, alu(V.kAdd, 6, 0xF, r(5), c(1)), alu(V.kAdd, 8, 0xF, r(6), r(2))]
    blob = build(body, [export(r(8))])
    check_rule("a loop", blob)
    nodes, _ = flow(blob)
    start = next(i for i, n in nodes.items() if isinstance(n["detail"], xu.ParsedLoopStartInstruction))
    end = next(i for i, n in nodes.items() if isinstance(n["detail"], xu.ParsedLoopEndInstruction))
    ops = ops_of(blob)
    fetched = next(op[2] for e, op in ops if e < start and op[0])
    top = next(op for e, op in ops if start < e < end)
    past = next(op for e, op in ops if e > end and op[3] & fetched)
    if top[0] or not top[1] or not past[1]:
        fail(f"a loop: the body's first instruction serialized {top[1]}, the read past it {past[1]}; want both, the "
             "first an ALU instruction")
    print("  PASS a loop: its first instruction waits for the fetch ending it on the pass before, and the read past it "
          "for the fetch before it")


def check_arms():
    """A fetch, then arms on the predicate each reading it: the first serialized, and the second too, as the first
    may not have run."""
    body = [tfetch(2, 0), {"predicate": 1}, alu(V.kMul, 4, 0xF, r(2), c(0)), {"predicate": 2},
            alu(V.kAdd, 4, 0xF, r(2), c(1)), {"predicate": 0}]
    blob = build(body, [export(r(4))])
    check_rule("arms on the predicate", blob)
    arms = [op[1] for _, op in ops_of(blob)[1:3]]
    if arms != [True, True]:
        fail(f"arms on the predicate: the arms' reads serialized {arms}; want both")
    print("  PASS arms on the predicate: each arm's read of the fetch before them is serialized")


def check_waiting_fetch_first():
    """Work, then two fetches at an input and one at the work's result: the one that waits for the work goes first, so
    it alone is serialized and the two after it don't wait for it."""
    body = [alu(V.kAdd, 2, 0xF, r(0), c(0)), tfetch(3, 1), tfetch(4, 1), tfetch(5, 2),
            alu(V.kAdd, 6, 0xF, r(3), r(4)), alu(V.kAdd, 7, 0xF, r(6), r(5))]
    blob = build(body, [export(r(7))])
    check_rule("a fetch waiting for the work", blob)
    ops = [op for _, op in ops_of(blob)]
    fetches = [op for op in ops if op[0]]
    work = set().union(*(op[2] for op in ops[:ops.index(fetches[0])]))
    if not fetches[0][3] & work or [f[1] for f in fetches] != [True, False, False]:
        fail(f"a fetch waiting for the work: fetches {fetches}; want the one reading the work first, it alone serialized")
    print("  PASS a fetch waiting for the stage's work goes before the fetches that don't, and alone waits")


def check_dependent_fetch():
    blob, manifest = lower_ex(module("vk_dependent_fetch.frag"), XSDK_REGISTERS)
    kinds = [(op[0], op[1]) for _, op in ops_of(blob)]
    if not any(s for f, s in kinds if f) or not any(s for f, s in kinds if not f):
        fail(f"vk_dependent_fetch.frag: serialized fetches and ALU {kinds}; want both kinds")
    print("  PASS vk_dependent_fetch.frag: serialized fetches and ALU instructions both")

    interface = resolve_interface(module("vk_dependent_fetch.frag"))
    slots = {slot: (var.descriptor_set, var.binding) for var, slot in interface.samplers}

    def fetch(slot, coordinates):
        if slots[slot] == (0, 0):
            return [INDEX / 255.0] * 4
        return [coordinates[0], coordinates[1], 0.0, 1.0]

    constants = push_constants("vk_dependent_fetch.frag", [SCALE])
    constants.update({reg: list(v) for reg, v in manifest})
    result = simulate.run(blob, registers={0: UV + [0.0, 0.0]}, constants=constants, texture_fetch=fetch)
    lookup = [(INDEX + 0.5) / 204.0, 0.5]
    check("vk_dependent_fetch.frag, the palette at the fetched index", result.exports[0],
          [lookup[0], lookup[1], 0.0, 1.0])


def check_lod_dependent():
    blob, _ = lower_ex(module("vk_lod_dependent.frag"), XSDK_REGISTERS)
    starts = setter_runs("vk_lod_dependent.frag", blob)
    c = container.Container(blob)
    serialized = [rec.serialize for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0] if rec.kind in SLOTS]
    if len(starts) < 3 or not serialized[min(starts)]:
        fail(f"vk_lod_dependent.frag: setter runs starting at {sorted(starts)}, serialized "
             f"{[serialized[i] for i in sorted(starts)]}; want three or more, the first waiting for the work")
    print(f"  PASS vk_lod_dependent.frag: {len(starts)} setter runs, the first waiting at its setter for the work its "
          "fetch reads, nothing waiting after a setter")


def main():
    check_testdata()
    check_later_exec()
    check_cleared()
    check_loop()
    check_arms()
    check_waiting_fetch_first()
    check_dependent_fetch()
    check_lod_dependent()
    print("OK")


if __name__ == "__main__":
    main()
