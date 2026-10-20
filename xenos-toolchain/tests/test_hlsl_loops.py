#!/usr/bin/env python3
"""Loops in SSA form (xsdk:docs/scope/50-hlsl.md, step 3). glslang runs spirv-opt's legalisation on HLSL, so a loop
arrives with Phis and its test in its header, its counter's step in the block branching back, and SSA values read
across its passes; xost puts it in the memory form glslang writes for GLSL, the shape the unroller takes
(xsdk:src/xsc_ssa.c): each Phi a Function variable stored on each edge into its block, the header's test in a block of
its own, a continue block holding the counter's step, a value the test works out worked out again where it's read, and
one a do/while's pass works out read after the loop from the variable its header's Phi takes it into.

Each shader of xsc/testdata/hlsl/loops runs on xdis/simulate.py beside its GLSL twin, <name>.frag, the optimiser off
and on, each pixel alone and all of them in one group, LOOP_END breaking once every one agrees, as the console does
(tests/test_hardware_loop_breaks.py): its colour must be the twin's bit for bit and the loop's exactly, on inputs no
operation rounds; its loops must take the twin's way, unrolled or hardware loops of the twin's kind; and it may be no
longer than the twin, in ALU and control flow instructions:

- counted_ps: a constant count, unrolled, an accumulator and the counter read in the pass;
- while_ps: a while the unroller counts, its counter stepped before a break on data, both read after the loop;
- break_ps: a constant count left on data, the sum and the counter read after the loop;
- exits_ps: a hardware loop an int in $Global counts, left on data, whose exits aren't its header's values alone: a
  value set before it, the sum, which a value read after the loop takes from the header too, and that value;
- nested_ps: nested loops, the inner one left on data;
- continue_ps: a continue on data;
- dowhile_ps: a do/while on data, a hardware loop of 255 passes, what its pass works out read after it;
- uniform_ps: a hardware loop an int in $Global counts, left on data, its counter read after it;
- data_ps: a while on data, whose test works out a value its pass and what follows read.

xsdk_test's HLSL grid's loop, hlsl/grid_loop_ps.hlsl (xsdk:tests/test_hlsl.c), a hardware loop an int in $Global
counts, runs too, its cell's 80 columns in one group, each pixel's colour the test's CPU reference's exactly.

Refused for what they are: refused_last_ps, a do/while's header's Phi read after the loop, where its variable holds
the next pass's value; refused_exit_ps, a value a loop's test works out kept past the loop, which a break sets too; and
refused_kept_ps, a value a do/while's pass works out read after the loop, which no Phi of its header carries.
"""
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402

OP_PHI, OP_LOOP_MERGE, OP_LABEL = 245, 246, 248


def counted(uv, _):
    s = list(uv)
    for i in range(5):
        s = [x * 0.5 + k * i for x, k in zip(s, COUNTED_STEP)]
    return s


def while_loop(uv, _):
    x, i = uv[0], 0
    while i < 6:
        i += 1
        x = x * 1.5 + 0.25
        if x > LIMIT[0]:
            break
    return [x, float(i), uv[1], 1.0]


def break_loop(uv, _):
    s, i = 0.0, 0
    while i < 8:
        s += uv[0] + i * 0.125
        if s > THRESHOLD[0]:
            break
        i += 1
    return [s, float(i), uv[1], 1.0]


def exits(uv, count):
    s = last = 0.0
    found = -1.0
    for i in range(count):
        last = s
        s += uv[0] + i * 0.25
        if s > THRESHOLD[0]:
            found = float(i)
            break
    return [s, last, found, 1.0]


def nested(uv, _):
    s = t = 0.0
    for i in range(3):
        for j in range(4):
            s += uv[0] * i + uv[1] * j * SCALE[0]
            if s > SCALE[1]:
                break
        t += s
    return [s, t, uv[2], 1.0]


def continue_loop(uv, _):
    s = 0.0
    for i in range(6):
        v = uv[0] * i
        if v > CONTINUE_LIMIT[0]:
            continue
        s += v
    return [s, uv[1], uv[2], uv[3]]


def do_while(uv, _):
    x, n = uv[0], 0
    while True:
        x = x * 1.5 + 0.25
        n += 1
        if not x < DO_LIMIT[0]:
            return [x, float(n), uv[1], 1.0]


def uniform(uv, count):
    s, i = list(uv), 0
    while i < count:
        s = [a + b for a, b in zip(s, UNIFORM_STEP)]
        if s[0] > UNIFORM_LIMIT[0]:
            break
        i += 1
    return s[:3] + [float(i)]


def data(uv, _):
    s, k = 0.0, 1.0
    while True:
        t = uv[0] * k
        if not t < DATA_LIMIT[0]:
            return [s, t, k, 1.0]
        s += t
        k += 1.0


COUNTED_STEP = [0.25, -0.5, 0.125, 1.0]
LIMIT = [3.0, 0.0, 0.0, 0.0]
THRESHOLD = [2.0, 0.0, 0.0, 0.0]
SCALE = [0.5, 3.0, 0.0, 0.0]
CONTINUE_LIMIT = [1.0, 0.0, 0.0, 0.0]
DO_LIMIT = [4.0, 0.0, 0.0, 0.0]
UNIFORM_STEP = [0.25, 0.5, -0.125, 0.0]
UNIFORM_LIMIT = [1.0, 0.0, 0.0, 0.0]
DATA_LIMIT = [2.0, 0.0, 0.0, 0.0]


def lanes(xs):
    return [[x, 0.375 - x / 4, 0.5 + x / 8, -0.25 * x] for x in xs]


# name -> (the loop, its constants from c0, uv's lanes, the counts its block loop runs, the twin's loops' kinds).
CASES = {
    "counted": (counted, [COUNTED_STEP], lanes([-1.0, 0.0, 0.25, 0.75, 2.0]), [None], []),
    "while": (while_loop, [LIMIT], lanes([-1.0, 0.0, 0.25, 0.5, 1.0, 2.0, 3.5]), [None], []),
    "break": (break_loop, [THRESHOLD], lanes([-0.5, 0.0, 0.125, 0.25, 0.5, 1.0, 3.0]), [None], []),
    "exits": (exits, [THRESHOLD], lanes([-0.5, 0.0, 0.125, 0.5, 1.0, 3.0]), [0, 2, 8], ["block_loop"]),
    "nested": (nested, [SCALE], lanes([-0.5, 0.0, 0.25, 0.5, 1.0, 1.5]), [None], []),
    "continue": (continue_loop, [CONTINUE_LIMIT], lanes([-0.25, 0.0, 0.125, 0.25, 0.375, 0.5]), [None], []),
    "dowhile": (do_while, [DO_LIMIT], lanes([0.0, 0.5, 1.0, 3.0, 4.0, 10.0]), [None], ["fixed_loop"]),
    "uniform": (uniform, [UNIFORM_STEP, UNIFORM_LIMIT], lanes([-1.0, -0.25, 0.0, 0.5, 0.75, 2.0]), [0, 1, 3, 8],
                ["block_loop"]),
    "data": (data, [DATA_LIMIT], lanes([0.25, 0.5, 0.75, 1.0, 3.0]), [None], ["fixed_loop"]),
}


def in_ssa_form(m):
    """Whether a block of the SPIR-V holds a Phi and a loop's merge: a loop's header in SSA form."""
    words = struct.unpack(f"<{len(m.blob) // 4}I", m.blob)[5:]
    seen = set()
    while words:
        opcode = words[0] & 0xFFFF
        if opcode == OP_LABEL:
            seen = set()
        seen.add(opcode)
        if {OP_PHI, OP_LOOP_MERGE} <= seen:
            return True
        words = words[words[0] >> 16:]
    return False


def program(name, optimize):
    m = module(f"hlsl/loops/{name}")
    blob, manifest = lower_ex(m, XSDK_REGISTERS, optimize)
    return m, blob, manifest


def constants(m, manifest, values):
    """The shader's block's registers from values, its literals, and c255."""
    block = next(b for b in resolve_interface(m).blocks if b.kind == "uniform")
    regs = {block.reg + i: list(v) for i, v in enumerate(values)}
    regs.update({entry[0]: list(entry[1]) for entry in manifest if isinstance(entry[0], int)})
    regs[native.FRAG_COORD_CONSTANT] = [0.5] * 4
    return regs


def loops(manifest, count):
    """Each loop constant's passes: a block loop's for count, as xsdk works them out, a fixed loop's own."""
    out = {}
    for entry in manifest:
        if entry[0] == "block_loop":
            out[entry[1]] = native.loop_passes(entry[6], entry[7], count)
        elif entry[0] == "fixed_loop":
            out[entry[1]] = entry[2]
    return out


def kinds(manifest):
    return sorted(entry[0] for entry in manifest if isinstance(entry[0], str))


def size(blob):
    """The program's ALU and control flow instructions."""
    parsed = container.Container(blob)
    records = xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0]
    return [sum(r.kind == kind for r in records) for kind in ("alu", "cf")]


def same(a, b):
    return all(x == y and math.copysign(1.0, x) == math.copysign(1.0, y) for x, y in zip(a, b))


def colours(blob, manifest, m, values, uvs, count):
    """Each lane's colour run alone, then in one group."""
    consts = constants(m, manifest, values)
    passes = loops(manifest, count)
    alone = [simulate.run(blob, {0: uv}, consts, loops=passes).exports[0] for uv in uvs]
    group = [r.exports[0] for r in simulate.run_group(blob, [{0: uv} for uv in uvs], consts, loops=passes)]
    return alone, group


def check(name, optimize):
    loop, values, uvs, counts, twin_kinds = CASES[name]
    tag = f"optimiser {'on' if optimize else 'off'}"
    hlsl = program(f"{name}_ps.hlsl", optimize)
    glsl = program(f"{name}.frag", optimize)
    if not in_ssa_form(hlsl[0]):
        raise SystemExit(f"  FAIL {name}_ps.hlsl: its SPIR-V holds no loop in SSA form, so the test can't tell")
    if kinds(hlsl[2]) != kinds(glsl[2]) or kinds(glsl[2]) != twin_kinds:
        raise SystemExit(f"  FAIL {name}_ps.hlsl, {tag}: loops {kinds(hlsl[2])}, the GLSL's {kinds(glsl[2])}, not "
                         f"{twin_kinds}")
    sizes = size(hlsl[1]), size(glsl[1])
    if any(a > b for a, b in zip(*sizes)):
        raise SystemExit(f"  FAIL {name}_ps.hlsl, {tag}: {sizes[0]} ALU and control flow instructions, the GLSL's "
                         f"{sizes[1]}")
    for count in counts:
        got = colours(hlsl[1], hlsl[2], hlsl[0], values, uvs, count)
        twin = colours(glsl[1], glsl[2], glsl[0], values, uvs, count)
        for lane, uv in enumerate(uvs):
            want = [simulate.single(v) for v in loop(uv, count)]
            runs = (("alone", got[0][lane], twin[0][lane]), ("in a group", got[1][lane], twin[1][lane]))
            for model, colour, glsl_colour in runs:
                if not same(colour, glsl_colour) or colour != want:
                    raise SystemExit(f"  FAIL {name}_ps.hlsl, {tag}, count {count}, lane {lane} {model}: {colour}, "
                                     f"the GLSL {glsl_colour}, the loop {want}")
    print(f"  PASS {name}_ps.hlsl, {tag}: {len(uvs)} lanes alone and in a group, counts {counts}, as its GLSL twin "
          f"and the loop, loops {twin_kinds or 'unrolled'}, {sizes[0]} ALU and control flow instructions, the GLSL's "
          f"{sizes[1]}")


# xsdk_test's HLSL grid's loop (xsdk:tests/test_hlsl.c): its step, its count, and its cell's columns.
GRID_STEP = [1.0 / 16, 1.0 / 32, 1.0 / 16, 0.0]
GRID_COUNT = 12
GRID_COLUMNS = 80


def grid_colour(u):
    c, i = [0.0, 0.0, 0.0, 1.0], 0
    while i < GRID_COUNT:
        c[0] += GRID_STEP[0]
        c[1] += GRID_STEP[1]
        if c[0] > u:
            break
        i += 1
    c[2] = i * GRID_STEP[2]
    return c


def grid(optimize):
    tag = f"optimiser {'on' if optimize else 'off'}"
    m = module("hlsl/grid_loop_ps.hlsl")
    blob, manifest = lower_ex(m, XSDK_REGISTERS, optimize)
    if kinds(manifest) != ["block_loop"]:
        raise SystemExit(f"  FAIL grid_loop_ps.hlsl, {tag}: loops {kinds(manifest)}, not a block's")
    uvs = [[(x + 0.5) / GRID_COLUMNS, 0.0, 0.0, 0.0] for x in range(GRID_COLUMNS)]
    group = simulate.run_group(blob, [{0: uv} for uv in uvs], constants(m, manifest, [GRID_STEP]),
                               loops=loops(manifest, GRID_COUNT))
    wrong = [x for x, r in enumerate(group) if r.exports[0] != [simulate.single(v) for v in grid_colour(uvs[x][0])]]
    if wrong:
        raise SystemExit(f"  FAIL grid_loop_ps.hlsl, {tag}: {len(wrong)} of {GRID_COLUMNS} columns wrong, column "
                         f"{wrong[0]} {group[wrong[0]].exports[0]}, not {grid_colour(uvs[wrong[0]][0])}")
    print(f"  PASS grid_loop_ps.hlsl, {tag}: all {GRID_COLUMNS} columns in one group, which ran {group[0].passes} "
          f"passes")


REFUSALS = {
    "refused_last_ps.hlsl": "a do/while's header's Phi read after the loop",
    "refused_exit_ps.hlsl": "a value a loop's test works out, kept past the loop",
    "refused_kept_ps.hlsl": "a value a loop's pass works out, read after the loop, other than one a do/while carries",
}


def refused():
    for name, why in REFUSALS.items():
        m = module(f"hlsl/loops/{name}")
        try:
            native.compile_spirv(m.blob, XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, True)
        except native.CompileError as e:
            if e.status != native.REFUSED or why not in str(e):
                raise SystemExit(f"  FAIL {name}: {e.status}, {e}")
        else:
            raise SystemExit(f"  FAIL {name} compiled")
        print(f"  PASS {name}: refused, {why}")


def main():
    refused()
    for optimize in (False, True):
        for name in CASES:
            check(name, optimize)
        grid(optimize)
    print("OK")


if __name__ == "__main__":
    main()
