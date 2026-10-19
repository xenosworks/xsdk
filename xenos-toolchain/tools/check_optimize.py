#!/usr/bin/env python3
"""check_optimize.py <shader or folder>...: compiles each GLSL shader, or SPIR-V file, with and without the compiler's
optimiser (xsdk:src/xsc_optimize.c), at xsdk's register split, and checks the two programs' microcode gives the same
exports, run on random inputs and constants by xdis/equivalence.py, on the export components the first defines and
the output's GLSL type has. Counts each program as xdis disassembles it: ALU instructions; copies, a vector max of one source with itself and no scalar
operation beside it; co-issued slots, a vector and a scalar operation in one instruction; texture and vertex fetches;
control flow instructions but cnop; and registers. Counts too the needless work milestone 48 goes after
(tools/needless.py), and with the optimiser on each copy's kind (tools/copy_kinds.py). Prints each shader's counts off
and on, then the totals per stage, and per stage of each --group, the shaders whose path matches one of its patterns;
exits 1 if any exports differ. A shader the compiler refuses is listed and left out. --json writes every count and
the totals, and --baseline prints how each total moved from such a file. Runs in WSL, as the compiler does, with xost's
Windows glslang.

    python3 tools/check_optimize.py ../local/m11-xenia-record/shaders --trials 20 --json counts.json \
        --group 'two=*/4ee5b3c0*,*/1510c05a*'
"""
import argparse
import collections
import fnmatch
import json
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container as container  # noqa: E402
import xdis.equivalence as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402

GLSLANG = ROOT / "tools" / "glslang" / "bin" / "glslang.exe"
NAMES = {"alu": "ALU", "copies": "copies", "co_issued": "co-issued", "fetches": "fetches",
         "control_flow": "control flow", "registers": "registers"}
# tools/needless.py's counts.
NEEDLESS = {"identities": "identities", "mix_pairs": "mix pairs", "divided_lanes": "divided lanes",
            "constant_rcps": "rcps of constants", "single_lane": "single-lane", "range_useless": "range-useless",
            "without_flow": "walked without flow"}
ALL_NAMES = {**NAMES, **NEEDLESS}


def host_path(path) -> str:
    """A path the Windows glslang can open from WSL."""
    if not sys.platform.startswith("linux"):
        return str(path)
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()


def inputs(blob):
    """The registers the microcode reads before it writes them: a pixel shader's interpolators and pixel parameters, a
    vertex shader's index."""
    c = container.Container(blob)
    written, read_first = set(), set()
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind == "alu":
            ins = xu.AluInstruction(*rec.raw)
            read_first |= {reg & 0x3F for reg in ins.src_temps().values()} - written
            if not ins.is_export():
                written |= {ins.vector_dest()} if ins.vector_write_mask() else set()
                written |= {ins.scalar_dest()} if ins.scalar_write_mask() else set()
        elif rec.kind in ("tfetch", "vfetch"):
            ins = (xu.TextureFetchInstruction if rec.kind == "tfetch" else xu.VertexFetchInstruction)(*rec.raw)
            read_first |= {ins.src()} - written
            written.add(ins.dest())
    return read_first


def counts(blob):
    """The program's counts, each instruction as the disassembly shows it: an operation it leaves out as a default nop
    isn't there."""
    c = container.Container(blob)
    n = dict.fromkeys(NAMES, 0)
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind == "alu":
            alu = rec.detail
            vector, scalar = not alu.IsVectorOpDefaultNop(), not alu.IsScalarOpDefaultNop()
            n["alu"] += 1
            n["co_issued"] += vector and scalar
            n["copies"] += (vector and not scalar and alu.vector_opcode == xu.AluVectorOpcode.kMax
                            and alu.vector_operands[0] == alu.vector_operands[1])
        elif rec.kind in ("tfetch", "vfetch"):
            n["fetches"] += 1
        elif rec.kind == "cf" and rec.opcode != "cnop":
            n["control_flow"] += 1
    regs = c.program_control & 0x3F if c.shader_type == "vertex" else (c.program_control >> 8) & 0x3F
    n["registers"] = regs + 1
    return n


def same(a, b):
    if a is None or b is None:
        return a is b
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return a == b


def differs(want, got, defined=None):
    """What differs between two runs' exports, on the components defined holds (want's that aren't None)."""
    defined = want if defined is None else defined
    if set(want) != set(got):
        return f"exports {sorted(want)} became {sorted(got)}"
    for reg in want:
        for i in range(4):
            if defined[reg][i] is not None and not same(want[reg][i], got[reg][i]):
                return f"export {reg}.{'xyzw'[i]} {want[reg][i]} became {got[reg][i]}"
    return None


def output_widths(m):
    """Each Location output's component count: the compiler exports Location n to export register n
    (xsdk:src/xsc_resolve.c, interp_out and color_out), and a component past the GLSL type's is one nothing writes."""
    location = {target: args[0] for target, decos in m.decorations.items() for deco, args in decos if deco == 30}
    widths = {}
    for ins in m.instrs:
        if ins.opcode != 59 or ins.operands[0] != 3 or ins.result_id not in location:  # OpVariable, Output
            continue
        pointee = m.by_result[m.by_result[ins.result_type].operands[1]]
        if pointee.opcode in (21, 22, 23):  # OpTypeInt, OpTypeFloat, OpTypeVector
            widths[location[ins.result_id]] = pointee.operands[1] if pointee.opcode == 23 else 1
    return widths


def compare(plain, optimized, manifest, manifest_opt, widths, trials, seed):
    """What differed between the two programs' exports, if anything, on the components each output's type has."""
    if manifest != manifest_opt:
        return "the literal constants differ"
    rng = random.Random(seed)
    for trial in range(trials):
        registers = {r: [rng.uniform(-2.0, 2.0) for _ in range(4)] for r in inputs(plain)}
        constants = {i: [rng.uniform(-2.0, 2.0) for _ in range(4)] for i in range(256)}
        for entry in manifest:
            if len(entry) == 2 and isinstance(entry[0], int):
                constants[entry[0]] = list(entry[1])
        try:
            want = simulate.run(plain, registers, constants)
            defined = {reg: [v if i < widths.get(reg, 4) else None for i, v in enumerate(values)]
                       for reg, values in want.items()}
            problem = differs(want, simulate.run(optimized, registers, constants), defined)
        except simulate.Unsupported as e:
            return f"not simulated: {e}"
        if problem:
            return f"trial {trial}: {problem}"
    return None


def check(spv_bytes, trials, seed):
    """(stage, counts off, counts on, problem): each program's counts, and what differed, if anything."""
    import copy_kinds  # both import this file
    import needless
    module = spirv.Module.parse(spv_bytes)
    plain, manifest = lower_ex(module, XSDK_REGISTERS)
    optimized, manifest_opt = lower_ex(spirv.Module.parse(spv_bytes), XSDK_REGISTERS, optimize=True)
    widths = output_widths(module)
    problem = compare(plain, optimized, manifest, manifest_opt, widths, trials, seed)
    before = {**counts(plain), **needless.count(plain, manifest)}
    after = {**counts(optimized), **needless.count(optimized, manifest_opt)}
    for kind, _, _, _ in copy_kinds.program_copies(optimized, manifest_opt, widths):
        after["kinds"]["copy: " + kind] = after["kinds"].get("copy: " + kind, 0) + 1
    return container.Container(plain).shader_type, before, after, problem


def spirv_of(source, work):
    if source.suffix == ".spv":
        return source.read_bytes()
    out = Path(work) / (source.name + ".spv")
    subprocess.run([str(GLSLANG), "-V", host_path(source), "-o", host_path(out)], check=True, capture_output=True)
    return out.read_bytes()


def total(shaders, stage):
    """A stage's totals: each count summed over its shaders, and the most registers one takes."""
    mine = [s for s in shaders.values() if s.get("stage") == stage]
    result = {"shaders": len(mine)}
    for program in ("off", "on"):
        result[program] = {k: sum(s[program][k] for s in mine) for k in ALL_NAMES}
        result[program]["registers_most"] = max((s[program]["registers"] for s in mine), default=0)
        kinds = sum((collections.Counter(s[program]["kinds"]) for s in mine), collections.Counter())
        result[program]["kinds"] = dict(sorted(kinds.items()))
    return result


def stage_totals(shaders):
    totals = {stage: total(shaders, stage) for stage in ("vertex", "pixel")}
    return {stage: t for stage, t in totals.items() if t["shaders"]}


def share(part, whole):
    return f"{100.0 * part / whole:.1f}%" if whole else "-"


def print_total(stage, t):
    off, on, count = t["off"], t["on"], t["shaders"]
    print(f"{stage}, {count} shaders: ALU {off['alu']} -> {on['alu']} ({share(off['alu'] - on['alu'], off['alu'])} "
          f"fewer); copies {off['copies']} ({share(off['copies'], off['alu'])} of ALU) -> {on['copies']} "
          f"({share(on['copies'], on['alu'])}); co-issued {off['co_issued']} ({share(off['co_issued'], off['alu'])}) "
          f"-> {on['co_issued']} ({share(on['co_issued'], on['alu'])}); fetches {off['fetches']} -> {on['fetches']}; "
          f"control flow {off['control_flow']} -> {on['control_flow']}; registers {off['registers'] / count:.1f} -> "
          f"{on['registers'] / count:.1f} a shader, most {off['registers_most']} -> {on['registers_most']}")
    print(f"{stage}, needless work: " + ", ".join(f"{name} {off[k]} -> {on[k]}" for k, name in NEEDLESS.items()))
    for kind in sorted(set(off["kinds"]) | set(on["kinds"])):
        print(f"    {kind}: {off['kinds'].get(kind, 0)} -> {on['kinds'].get(kind, 0)}")


def print_baseline(totals, baseline, label=""):
    """Each total against the baseline's, the optimiser off and on; a count the baseline hasn't is left out."""
    for stage, t in totals.items():
        if stage not in baseline:
            continue
        for program in ("off", "on"):
            was, now = baseline[stage][program], t[program]
            moves = ", ".join(f"{ALL_NAMES.get(k, 'most registers')} {was[k]} -> {now[k]}"
                              + (f" ({100.0 * (now[k] - was[k]) / was[k]:+.1f}%)" if was[k] else "")
                              for k in now if k in was and k != "kinds")
            print(f"against the baseline, {label}{stage}, optimiser {program}: {moves}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json", help="write every shader's counts and the totals to this file")
    ap.add_argument("--baseline", help="a file --json wrote, whose totals these are compared with")
    ap.add_argument("--group", action="append", default=[], metavar="NAME=PATTERN[,PATTERN...]",
                    help="totals too for the shaders whose path matches a pattern, as fnmatch matches")
    args = ap.parse_args()
    sources = []
    for p in map(Path, args.paths):
        sources += sorted(q for q in p.iterdir() if q.suffix in (".vert", ".frag")) if p.is_dir() else [p]
    shaders = {}
    failed = 0
    skipped = 0
    refused = 0
    with tempfile.TemporaryDirectory() as work:
        for source in sources:
            try:
                stage, before, after, problem = check(spirv_of(source, work), args.trials, args.seed)
            except (LowerError, ValueError) as e:
                refused += 1
                shaders[str(source)] = {"result": f"not compiled: {e}"}
                print(f"{source.name}: not compiled: {e}")
                continue
            if problem and problem.startswith("not simulated"):
                skipped += 1
            elif problem:
                failed += 1
            shaders[str(source)] = {"stage": stage, "off": before, "on": after, "result": problem or "ok"}
            print(f"{source.name}: " + ", ".join(f"{ALL_NAMES[k]} {before[k]} -> {after[k]}" for k in ALL_NAMES)
                  + f"; {problem or 'ok'}")
    totals = stage_totals(shaders)
    for stage, t in totals.items():
        print_total(stage, t)
    groups = {}
    for spec in args.group:
        name, patterns = spec.split("=", 1)
        mine = {p: s for p, s in shaders.items() if any(fnmatch.fnmatch(p, q) for q in patterns.split(","))}
        groups[name] = stage_totals(mine)
        for stage, t in groups[name].items():
            print_total(f"{name}, {stage}", t)
    print(f"{len(sources)} shaders, {failed} differ, {skipped} not simulated, {refused} not compiled")
    if args.baseline:
        baseline = json.loads(Path(args.baseline).read_text())
        print_baseline(totals, baseline["totals"])
        for name, t in groups.items():
            print_baseline(t, baseline.get("groups", {}).get(name, {}), f"{name}, ")
    if args.json:
        Path(args.json).write_text(json.dumps({"trials": args.trials, "seed": args.seed, "shaders": shaders,
                                               "totals": totals, "groups": groups}, indent=1) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
