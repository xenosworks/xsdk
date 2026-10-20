#!/usr/bin/env python3
"""check_hlsl.py [folder]...: the HLSL path of xsdk milestone 50 (xsdk:docs/scope/50-hlsl.md). Each .hlsl in the
folders, xsc/testdata/hlsl's corpus and idioms by default, goes through glslang as a port's shader model 3 code would,
-D -V --hlsl-dx9-compatible -fhlsl_functionality1 -S <stage> -e <entry>, the last flag keeping each varying's semantic
as a string xost reads (SPV_GOOGLE_hlsl_functionality1), the stage from the name's _vs or _ps and the entry vs_main or
ps_main where the source has one, else main, then through xost at xsdk's register split with the optimiser on. What
xost refuses is sorted by cause.

A corpus shader beside its GLSL twin, <name>.frag for <name>_ps.hlsl and <name>.vert for <name>_vs.hlsl, is compared
with it: the twin goes through glslang -V and xost as GLSL does today, and both programs run on xdis/simulate.py on
the same random inputs, constants and textures, matched by name: a pixel shader's interpolators, and VPOS and
gl_FragCoord the same pixel; a vertex shader's fetches, each told apart by the input its element names. Each stage is
compared alone, a varying at the interpolator xost gives it in that stage (xsc/resolve.py), an HLSL one packed by
semantic, so the pair's link at bind (xsdk:src/shader.c, xsdk_shader_link) doesn't enter. HLSL's
constants sit at their registers, as D3D9's SetPixelShaderConstantF sets them (glslang's $Global block, which xost
lays from c0), the GLSL's in its blocks; a uniform <block>_<member> of the HLSL is member <member> of the GLSL's block
instance <block>, as SPIRV-Cross names them. Each comparison is exact, every export component the shader's type has,
a vertex shader's position among them, equal bit for bit on every trial, inexact, or not comparable, with the reason.

--refresh writes each shader's SPIR-V beside it, <name>.spv, as testdata keeps it; --json writes every result. Runs in
WSL with xost's Windows glslang, as check_optimize.py does.

    python3 tools/check_hlsl.py --json ../results/2026-10-20-milestone-50/step2.json
"""
import argparse
import collections
import json
import math
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
import xsc.resolve as resolve  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from check_optimize import GLSLANG, host_path  # noqa: E402

TESTDATA = ROOT / "xsc" / "testdata" / "hlsl"
FLAGS = ["-D", "-V", "--hlsl-dx9-compatible", "-fhlsl_functionality1"]
# xsdk's split (xsdk:src/xsc_compile.c).
REGISTERS = 64
ENTRY = re.compile(r"\b(vs_main|ps_main)\s*\(")
# The constant xsdk keeps for gl_FragCoord and VPOS outside a tiled pass (xsdk:include/xsdk/xsc.h,
# XSDK_XSC_FRAG_COORD_CONSTANT).
FRAG_COORD = (0.5, 0.5, 0.0, 0.0)
# A vertex shader's position export, and a fetch's constant fields (xenia:src/xenia/gpu/ucode.h:745-758).
EXPORT_POSITION = 62
CONSTANT_SHIFT, SELECT_SHIFT = 20, 25
CONSTANT_FIELDS = 0x7F << CONSTANT_SHIFT
OP_LOAD, OP_ACCESS_CHAIN, OP_VARIABLE, OP_UNDEF, OP_CONSTANT_NULL, OP_CONSTANT_COMPOSITE = 61, 65, 59, 1, 46, 44
STORAGE_PRIVATE = 6
DEC_OFFSET, DEC_ARRAY_STRIDE, DEC_MATRIX_STRIDE, DEC_BUILTIN, BUILTIN_FRAG_COORD = 35, 6, 7, 11, 15


def spirv_of(source, args, work):
    out = Path(work) / (source.name + ".spv")
    result = subprocess.run([str(GLSLANG), *args, host_path(source), "-o", host_path(out)], capture_output=True,
                            text=True)
    if result.returncode:
        raise ValueError("glslang: " + (result.stdout + result.stderr).strip().splitlines()[-1])
    return out.read_bytes()


def hlsl_args(source):
    stage = "vert" if source.stem.endswith("_vs") else "frag"
    found = ENTRY.search(source.read_text())
    return FLAGS + ["-S", stage, "-e", found.group(1) if found else "main"]


def defined_by(module, ident):
    """The instruction defining an id, OpUndef and OpConstantNull too, which xsc/spirv.py doesn't index."""
    found = module.by_result.get(ident)
    if found:
        return found
    return next((i for i in module.instrs if i.opcode in (OP_UNDEF, OP_CONSTANT_NULL) and i.words[1] == ident), None)


def opcode_of(module, ident):
    ins = defined_by(module, ident)
    return ins.opcode if ins else None


def private_load(module, ident):
    """Whether ident is a Private variable or a load of one, through an access chain too."""
    ins = defined_by(module, ident)
    while ins and ins.opcode in (OP_LOAD, OP_ACCESS_CHAIN):
        ins = module.by_result.get(ins.operands[0])
    return bool(ins and ins.opcode == OP_VARIABLE and ins.operands[0] == STORAGE_PRIVATE)


def float2_frag_coord(module, _):
    """Whether the module's FragCoord is a float2, as HLSL may declare SV_Position."""
    for target, decorations in module.decorations.items():
        if (DEC_BUILTIN, (BUILTIN_FRAG_COORD,)) in decorations:
            pointer = module.by_result[module.by_result[target].result_type]
            return width(module, pointer.operands[1]) == 2
    return False


# What xost's messages mean, the first that fits; a check, where there is one, must hold for an id the message names
# (any id for float2_frag_coord).
CAUSES = (
    (r"unresolved Load|cannot resolve scalar", private_load, "a static global (Private variable)"),
    (r"is not a constant scalar", lambda m, n: opcode_of(m, n) == OP_CONSTANT_COMPOSITE,
     "a splat constant (OpConstantComposite)"),
    (r"gl_FragCoord\.zw", float2_frag_coord, "a float2 FragCoord used whole"),
    (r"gl_FragCoord\.zw", None, "FragCoord's z or w"),
    (r"no write to gl_Position", None, "D3D9 semantics: POSITION out a location (step 4)"),
    (r"declare inputs in r0", None, "D3D9 semantics: inputs, VPOS or VFACE among them, as locations (step 4)"),
    (r"a colour output at location \d+, past oC3", None, "D3D9 semantics: DEPTH out a colour's location (step 4)"),
    (r"loop header terminator|structured loop header", None, "a loop in SSA form (step 3)"),
    (r"branch back|a loop's test works out|a loop's pass works out|do/while's header's Phi|edge from a loop's test",
     None, "a loop in SSA form xost doesn't reshape (step 3)"),
    (r"indexed access into a local|local array|a constant array of other than", None,
     "a local array other than a constant one at a run-time index, or written by element"),
    (r"integer Load must be a foldable local", None,
     "an int or bool other than a local's, an input's or HLSL's block's"),
    (r"unhandled matrix op|matrix operand must be a uniform Load|a matrix held by rows", None,
     "a matrix other than a uniform's, a pixel shader's input, or one made of columns"),
    (r"unsupported integer op", None, "an int operation xost doesn't emulate with floats"),
    (r"not one of D3D9's declaration usages|share semantic|without a semantic beside|more than \d+ interpolators", None,
     "D3D9 semantics: one xost doesn't link"),
    (r"with an initializer", None, "a local with an initializer"),
)


def cause_of(message, module):
    ids = [int(n) for n in re.findall(r"%(\d+)", message)] or [0]
    if any(opcode_of(module, n) in (OP_UNDEF, OP_CONSTANT_NULL) for n in ids):
        return "an OpUndef or OpConstantNull"
    for pattern, holds, cause in CAUSES:
        if re.search(pattern, message) and (holds is None or any(holds(module, n) for n in ids)):
            return cause
    return "other: " + re.sub(r"\d+", "N", message)


def compile_xost(blob):
    """(container, manifest) or the refusal's message."""
    try:
        container, manifest, _ = native.compile_spirv(blob, REGISTERS, REGISTERS, True, True)
    except native.CompileError as e:
        return None, None, str(e)
    return container, manifest, None


def short(name):
    """An interface variable's name without glslang's entry point prefix: stage_input.in_uv is in_uv."""
    return name.rsplit(".", 1)[-1]


def width(module, type_id):
    t = module.by_result.get(type_id)
    return t.operands[1] if t and t.opcode == 23 else 1


def type_bytes(module, type_id, matrix_stride):
    """A block member's bytes, as std140 lays it (its decorations say the strides)."""
    t = module.by_result[type_id]
    if t.opcode in (21, 22):
        return 4
    if t.opcode == 23:
        return 4 * t.operands[1]
    if t.opcode == 24:
        return matrix_stride * t.operands[1]
    if t.opcode == 28:
        stride = dict(module.decorations.get(type_id, ())).get(DEC_ARRAY_STRIDE)
        length = module.by_result[t.operands[1]].operands[0]
        if stride:
            return stride[0] * length
    raise ValueError(f"a block member of {t.name}")


def value(seed, key, count, low=-2.0):
    """count random singles from low to 2, as the GPU holds constants and inputs."""
    rng = random.Random(f"{seed}:{key}")
    return [simulate.single(rng.uniform(low, 2.0)) for _ in range(count)]


def constants(module, si, manifest, name_of, seed):
    """Each block member's words at its registers, a member's from its name; then the literals, and c255."""
    regs = {}
    for block in si.blocks:
        struct = module.by_result[block.struct]
        for i, member in enumerate(struct.operands):
            decorations = dict(module.member_decorations.get((block.struct, i), ()))
            offset = decorations[DEC_OFFSET][0]
            stride = decorations.get(DEC_MATRIX_STRIDE, (16,))[0]
            key = name_of(block.var.name, module.member_names.get((block.struct, i), str(i)))
            words = value(seed, key, type_bytes(module, member, stride) // 4)
            for w, v in enumerate(words):
                reg = block.reg + (offset + 4 * w) // 16
                regs.setdefault(reg, [0.0] * 4)[(offset + 4 * w) % 16 // 4] = v
    for entry in manifest:
        if len(entry) == 2 and isinstance(entry[0], int):
            regs[entry[0]] = list(entry[1])
    regs[native.FRAG_COORD_CONSTANT] = list(FRAG_COORD)
    return regs


def interface(module):
    """A pixel shader's interface by name: inputs (name -> register), exports (name -> (export, width)), samplers
    (slot -> name). gl_FragCoord, VPOS and the facing are the pixel parameters, the register past the interpolators
    (docs/xsc.md), the same pixel's for both programs."""
    si = resolve.resolve_interface(module)
    inputs = {short(v.name): r for v, r, _ in si.interp_in}
    exports = {short(v.name): (n, width(module, v.pointee)) for v, n in si.color_out}
    if si.frag_coord or si.vpos or si.front_facing or si.vface or si.point_coord:
        inputs["@frag_coord"] = max(inputs.values(), default=-1) + 1
    samplers = {slot: short(v.name) for v, slot in si.samplers}
    return si, inputs, exports, samplers


def texel(samplers, seed):
    def sample(slot, coords, lod):
        """A smooth function of the coordinates and the LOD, its own for each sampler's name."""
        rng = random.Random(f"{seed}:texture:{samplers.get(slot, slot)}")
        k = [[rng.uniform(-3.0, 3.0) for _ in range(3)] for _ in range(4)]
        level = lod.bias + (lod.register or 0.0) + (0.5 if lod.computed else 0.0)
        return [math.sin(sum(a * c for a, c in zip(row, coords)) + 0.37 * level + i) for i, row in enumerate(k)]
    return sample


def vertex_elements(blob):
    """A vertex shader's elements, (slot, usage, usage index) each, as xsdk:src/shader.c patches its fetches by them:
    the record's count, then the elements four words on (tests/test_optimize.py)."""
    words = struct.unpack(f">{len(blob) // 4}I", blob)
    record = words[6] // 4
    return [(w & 0xFFF, w >> 12 & 0xF, w >> 16 & 0xF) for w in words[record + 10:record + 10 + words[record + 7]]]


def separated(blob, elements):
    """The blob with element n's fetch at fetch constant n, so a fetch says which input it reads."""
    c = container.Container(blob)
    out = bytearray(blob)
    for n, (slot, _, _) in enumerate(elements):
        at = c.offset + c.sh_off + slot * 12
        word = struct.unpack_from(">I", out, at)[0] & ~CONSTANT_FIELDS
        struct.pack_into(">I", out, at, word | n // 3 << CONSTANT_SHIFT | n % 3 << SELECT_SHIFT)
    return bytes(out)


def run_vertex(blob, module, manifest, name_of, trial, seed):
    """A vertex shader's exports by name, its position "@position", and its inputs' names."""
    si = resolve.resolve_interface(module)
    by_usage = {(usage, index): short(v.name) for v, usage, index in si.vfetch}
    elements = vertex_elements(blob)
    names = [by_usage[(usage, index)] for _, usage, index in elements]
    # From 0, so an index a shader works out from an input stays in its array, as GX's matrix indices do.
    data = {name: value(f"{seed}:{trial}", name, 4, 0.0) for name in names}
    result = simulate.run(separated(blob, elements), {0: [0.0] * 4}, constants(module, si, manifest, name_of, seed),
                          vertex_fetch=lambda n, fetch, index: data[names[fetch.fetch_constant_index()]])
    exports = {short(v.name): (o, width(module, v.pointee)) for v, o, _ in si.interp_out}
    exports["@position"] = (EXPORT_POSITION, 4)
    missing = [n for n, _ in exports.values() if n not in result.exports]
    if missing:
        raise ValueError(f"no export {missing}")
    return {name: result.exports[n][:w] for name, (n, w) in exports.items()}, False, set(names)


def run(blob, module, manifest, name_of, trial, seed):
    """A shader's exports by name, whether it killed, and its inputs' names."""
    if container.Container(blob).shader_type == "vertex":
        return run_vertex(blob, module, manifest, name_of, trial, seed)
    si, inputs, exports, samplers = interface(module)
    consts = constants(module, si, manifest, name_of, seed)
    trial_seed = f"{seed}:{trial}"
    registers = {r: value(trial_seed, name, 4) for name, r in inputs.items() if name != "@frag_coord"}
    if "@frag_coord" in inputs:
        rng = random.Random(f"{trial_seed}:@frag_coord")
        registers[inputs["@frag_coord"]] = [float(rng.randrange(1280)), float(rng.randrange(720)), 0.0, 0.0]
    result = simulate.run(blob, registers, consts, texture_lod=texel(samplers, seed))
    missing = [n for n, _ in exports.values() if n not in result.exports]
    if missing:
        raise ValueError(f"no export {missing}")
    return {name: result.exports[n][:w] for name, (n, w) in exports.items()}, result.killed, set(inputs)


def same(a, b):
    """Bit for bit, a zero's sign too, any NaN as any other."""
    return (math.isnan(a) and math.isnan(b)) or (a == b and math.copysign(1.0, a) == math.copysign(1.0, b))


def compare(hlsl_blob, hlsl_module, hlsl_manifest, glsl_blob, glsl_module, glsl_manifest, trials, seed):
    """("exact" | "inexact" | "not comparable", detail)."""
    worst = None
    for trial in range(trials):
        try:
            want, want_kill, glsl_inputs = run(glsl_blob, glsl_module, glsl_manifest, lambda b, m: f"{b}_{m}",
                                               trial, seed)
            got, got_kill, hlsl_inputs = run(hlsl_blob, hlsl_module, hlsl_manifest, lambda b, m: m, trial, seed)
        except (NotImplementedError, RuntimeError) as e:
            return "not comparable", f"not simulated: {e}"
        except ValueError as e:
            return "not comparable", f"not matched: {e}"
        # glslang's optimiser drops an input the HLSL never reads.
        if not hlsl_inputs <= glsl_inputs or set(want) != set(got):
            return "not comparable", (f"interfaces differ: inputs {sorted(glsl_inputs)} and {sorted(hlsl_inputs)}, "
                                      f"exports {sorted(want)} and {sorted(got)}")
        if want_kill != got_kill:
            return "inexact", f"trial {trial}: kill {want_kill} against {got_kill}"
        for name in want:
            for a, b in zip(want[name], got[name]):
                if not same(a, b):
                    worst = max(worst or 0.0, math.inf if math.isnan(a) or math.isnan(b) else abs(a - b))
    return ("exact", "") if worst is None else ("inexact", f"largest difference {worst:.3g}")


def check(source, args, work):
    """One shader's result: its status, cause, message and, for a corpus shader, the comparison with its twin."""
    vertex = source.stem.endswith("_vs")
    twin = source.with_name(source.stem[:-3] + (".vert" if vertex else ".frag"))
    twin = twin if twin.exists() else None
    try:
        blob = spirv_of(source, hlsl_args(source), work)
        twin_blob = spirv_of(twin, ["-V"], work) if twin else None
    except ValueError as e:
        return {"status": "glslang", "cause": str(e)}
    if args.refresh:
        source.with_name(source.name + ".spv").write_bytes(blob)
        if twin:
            twin.with_name(twin.name + ".spv").write_bytes(twin_blob)
    module = spirv.Module.parse(blob)
    compiled, manifest, message = compile_xost(blob)
    if message:
        result = {"status": "refused", "cause": cause_of(message, module), "message": message}
    else:
        result = {"status": "compiled"}
    if not twin:
        return result
    if result["status"] != "compiled":
        result["comparison"] = ("not comparable", result["cause"])
    else:
        twin_container, twin_manifest, twin_message = compile_xost(twin_blob)
        result["comparison"] = (("not comparable", f"the GLSL refused: {twin_message}") if twin_message else
                                compare(compiled, module, manifest, twin_container, spirv.Module.parse(twin_blob),
                                        twin_manifest, args.trials, args.seed))
    return result


def report(name, results):
    counts = collections.Counter(r["status"] for r in results.values())
    print(f"== {name}: {len(results)} shaders, " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    causes = collections.Counter(r["cause"] for r in results.values() if r.get("cause"))
    for cause, count in causes.most_common():
        print(f"  {count:4d}  {cause}")
    comparisons = [r["comparison"] for r in results.values() if "comparison" in r]
    if comparisons:
        kinds = collections.Counter(c[0] for c in comparisons)
        print("  compared with the GLSL path: " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
        reasons = collections.Counter(c[1] for c in comparisons if c[0] == "not comparable")
        for reason, count in reasons.most_common():
            print(f"  {count:4d}  not comparable: {reason}")
    return {"counts": dict(counts), "causes": dict(causes),
            "comparisons": dict(collections.Counter(c[0] for c in comparisons))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folders", nargs="*", default=[TESTDATA / "corpus", TESTDATA / "idioms"], type=Path)
    ap.add_argument("--trials", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--refresh", action="store_true", help="write each shader's SPIR-V beside it")
    ap.add_argument("--json", help="write every shader's result and the totals to this file")
    args = ap.parse_args()
    everything = {}
    with tempfile.TemporaryDirectory() as work:
        for folder in args.folders:
            results = {}
            for source in sorted(folder.glob("*.hlsl")):
                results[source.name] = check(source, args, work)
                r = results[source.name]
                print(f"{source.name}: {r['status']}" + (f", {r['cause']}" if r.get("cause") else "")
                      + (f"; {' '.join(r['comparison']).strip()}" if "comparison" in r else ""))
            everything[folder.name] = {"shaders": results, "totals": report(folder.name, results)}
    if args.json:
        Path(args.json).write_text(json.dumps({"trials": args.trials, "seed": args.seed, "sets": everything},
                                              indent=1) + "\n")


if __name__ == "__main__":
    main()
