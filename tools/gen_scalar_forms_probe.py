#!/usr/bin/env python3
"""gen_scalar_forms_probe.py <out.h>: the cases of probe_scalar_forms, the console probe of the scalar unit's forms that
milestone 48 step 5's co_issue writes (results/2026-10-19-milestone-48/README.md, "Step 5"): mulsc, addsc and subsc
of a constant and a temp, adds, subs, muls, maxs and mins of two lanes of one register, floors, frcs, truncs and maxs
copies of one lane, and rcp of a constant as the control. The console gave wrong pictures where mulsc and addsc read
odd temps; the probe showed the cause, a constant form whose constant's lane is its temp's reading the temp for both
(xsdk:docs/HARDWARE.md, "The scalar unit's constant forms, milestone 48 step 5"), which its cases now expect.

Each case is a pixel shader that loads r0 to r63 from c128 to c191, runs the instruction under test once, alone or
beside a vector op, and exports r61.x, which the scalar op writes, or r60.x, which the vector op writes, to a 32-bit
float target. The cases share one container; the probe puts each case's instruction and export into a copy of it.
Every lane of every input holds its own value, so a value the GPU gets wrong names what it read:
- r[k]'s lane l holds 8k + LANE_CODES[l] + 1 + (2l + 1) / 8: floor names the register and lane, frc the lane, and two
  lanes' sums, differences and products name the pair, the lane codes being a Golomb ruler;
- c0, c1, c13, c105 and c2 hold powers of two, a lane each: a product's exponent names the constant and its mantissa the
  temp, and a sum keeps both, every one exact.
The generator stops unless xost's simulator (xenos-toolchain/xdis/simulate.py, Xenia's model but for the lanes above)
runs each case to a value that only the case's own reading of its inputs gives, by the probe's search, or, for a temp
with itself, which the search can't single out, to that value; unless the disassembler reads each instruction as the
case names it; and unless the probe's patching rebuilds each case's container word for word.
"""
import itertools
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import gen_triangle_shaders as tri  # noqa: E402
import gen_xsc_probe as pack  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
from gen_triangle_shaders import ce, enc, xu  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
LANES = "xyzw"
X, Y, Z, W = range(4)
REGISTERS = 64
TEMP_ROW = 128
CONSTANT_ROWS = TEMP_ROW + REGISTERS
LANE_CODES = (0, 1, 4, 6)
# The named constants' lanes as powers of two: from 2^10, past twice the largest temp, to 2^20, where a sum still keeps
# a temp's eighths; from 2^-4, below a temp's eighths, to 2^-12, which a sum keeps too.
EXPONENTS = {0: (10, 11, 12, 13), 1: (14, 15, 16, 17), 2: (-9, -10, -11, -12), 13: (18, 19, 20, -4),
             105: (-5, -6, -7, -8)}
SCALAR_DEST, VECTOR_DEST = 61, 60
EXEC_MOST = 6
# What the probe's resolve clears to, which no case gives.
CLEAR = -1234567.0

# The vector half of the instruction: its fields, its disassembly, and the op and reading of what it writes to r60.x.
VARIANTS = {
    "alone": (dict(vector_opcode=V.kMax, src1_reg=12, src2_reg=13), "max r0._, r12, r13", None, None),
    "add": (dict(vector_opcode=V.kAdd, vector_dest=VECTOR_DEST, vector_write_mask=0xF, src1_reg=10, src2_reg=11),
            "add r60, r10, r11", "OP_ADD", "r10.x + r11.x"),
    "mul c2": (dict(vector_opcode=V.kMul, vector_dest=VECTOR_DEST, vector_write_mask=0xF, src1_reg=10, src2_reg=2,
                    src2_is_temp=False), "mul r60, r10, c2", "OP_MUL", "r10.x * c2.x"),
    # src2 reads c2's y, z, w and x (xenia:src/xenia/gpu/ucode.h:1977-1982), so the instruction reads three constants.
    "add c2 twice": (dict(vector_opcode=V.kAdd, vector_dest=VECTOR_DEST, vector_write_mask=0xF, src1_reg=2,
                          src1_is_temp=False, src2_reg=2, src2_is_temp=False, src2_swizzle=0x55),
                     "add r60, c2, c2.yzwx", "OP_ADD", "c2.x + c2.y"),
}
CO_ISSUED = ("alone", "add")
VECTOR_HALF = "vector half"
FORMS = ("mulsc", "addsc", "subsc", "adds", "subs", "muls", "maxs", "mins", "floors", "frcs", "truncs", "maxs copy",
         "rcp, the control", VECTOR_HALF)
CONSTANT_OPS = (("mulsc", S.kMulsc0, "OP_MUL"), ("addsc", S.kAddsc0, "OP_ADD"), ("subsc", S.kSubsc0, "OP_ADD"))
LANE_OPS = (("adds", S.kAdds, "OP_ADD"), ("subs", S.kSubs, "OP_ADD"), ("muls", S.kMuls, "OP_MUL"),
            ("maxs", S.kMaxs, "OP_PICK"), ("mins", S.kMins, "OP_PICK"))
UNARY_OPS = (("floors", S.kFloors, "OP_FLOOR"), ("frcs", S.kFrcs, "OP_FRC"), ("truncs", S.kTruncs, "OP_TRUNC"),
             ("maxs copy", S.kMaxs, "OP_PICK"))
# Each bit of a temp's index alone, the ones the console got wrong (r1, r5, r9) and all six bits.
TEMPS = (0, 1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 63)
NAMED = (0, 1, 13, 105)
ODD_REGISTER, EVEN_REGISTER = 7, 6
FOUR_PAIRS = ((W, X), (X, Y), (Z, W), (Y, Z))


def temp_value(k, lane):
    return 8 * k + LANE_CODES[lane] + 1 + (2 * lane + 1) / 8


def constant_value(row, lane):
    return 2.0 ** EXPONENTS[row][lane]


def constants():
    """Every row the probe sets, c0 to c191: the named constants, the temps' rows, zeros between."""
    rows = {n: [0.0] * 4 for n in range(CONSTANT_ROWS)}
    for row in EXPONENTS:
        rows[row] = [constant_value(row, lane) for lane in range(4)]
    for k in range(REGISTERS):
        rows[TEMP_ROW + k] = [temp_value(k, lane) for lane in range(4)]
    return rows


def inputs():
    """The values the probe names, as it lists them: each temp's lanes, then each named constant's."""
    named = [(f"r{k}.{LANES[lane]}", temp_value(k, lane)) for k in range(REGISTERS) for lane in range(4)]
    return named + [(f"c{row}.{LANES[lane]}", constant_value(row, lane)) for row in sorted(EXPONENTS)
                    for lane in range(4)]


def swizzle(a, b):
    """src3's swizzle as xost writes a scalar op's (src/xsc_optimize.c, scalar_form): lane w reads a, the others b, each
    component-relative (xenia:src/xenia/gpu/ucode.h:1977-1982)."""
    return sum(((b if lane < 3 else a) - lane) % 4 << 2 * lane for lane in range(4))


def constant_form(opcode, row, a, temp, b, negate):
    """mulsc, addsc or subsc of c[row]'s lane a and r[temp]'s lane b, the temp's index spread over the opcode's bit 0,
    src3_sel and the swizzle's bits 2 to 5 as xost's encode spreads it (src/xsc_program.c)."""
    return dict(scalar_opcode=S(int(opcode) | temp & 1), src3_is_temp=bool(temp >> 1 & 1),
                src3_swizzle=swizzle(a, b) & 0xC3 | temp & 0x3C, src3_reg=row, src3_negate=negate)


def lanes_form(opcode, reg, a, b, negate=False, is_temp=True):
    return dict(scalar_opcode=opcode, src3_reg=reg, src3_swizzle=swizzle(a, b), src3_negate=negate,
                src3_is_temp=is_temp)


class Case:
    """One case: its form, the probe's op and search domain, the instruction's fields, what the disassembler reads in
    its scalar half, the vector half, which register it exports, and the reading Xenia's model gives."""

    def __init__(self, form, op, domain, fields, text, variant, reading, exported=SCALAR_DEST):
        self.form, self.op, self.domain, self.fields, self.text = form, op, domain, fields, text
        self.variant, self.reading, self.exported = variant, reading, exported
        # A temp with itself, which the probe's search can't single out: twice it is a difference of two lanes, and
        # less itself 0.
        self.alike = None
        if exported == VECTOR_DEST:
            self.form, self.op, self.domain, self.reading = VECTOR_HALF, VARIANTS[variant][2], "DOMAIN_ALL", \
                VARIANTS[variant][3]

    def name(self):
        vector = VARIANTS[self.variant][1]
        where = "alone" if self.variant == "alone" else f"beside {vector}"
        what = ", r60.x exported" if self.exported == VECTOR_DEST else ""
        return f"{self.text} {where}{what}"

    def instruction(self):
        return enc.build_alu(**VARIANTS[self.variant][0], scalar_dest=SCALAR_DEST, scalar_write_mask=1, **self.fields)


def neg(negate):
    return "-" if negate else ""


def constant_case(op, row, a, temp, b, negate, variant, exported=SCALAR_DEST):
    form, opcode, family = op
    c, r = f"c{row}.{LANES[a]}", f"r{temp}.{LANES[b]}"
    # The console reads the temp for the constant too where their lanes match (xsdk:docs/HARDWARE.md, "The scalar
    # unit's constant forms, milestone 48 step 5").
    first = r if a == b else c
    # One negation for both operands, as Xenia takes it (xenia:src/xenia/gpu/ucode.h:961-962).
    reading = {"mulsc": f"{first} * {r}", "addsc": f"{neg(negate)}{first} {'-' if negate else '+'} {r}",
               "subsc": f"{neg(negate)}{first} {'+' if negate else '-'} {r}"}[form]
    case = Case(form, family, "DOMAIN_CONSTANT_TEMP", constant_form(opcode, row, a, temp, b, negate),
                f"{form} {neg(negate)}{c}, {neg(negate)}{r}", variant, reading, exported)
    if a == b and exported == SCALAR_DEST:
        t = -temp_value(temp, b) if negate else temp_value(temp, b)
        case.alike = {"mulsc": t * t, "addsc": t + t, "subsc": t - t}[form]
    return case


def lane_name(reg, lane):
    return f"r{reg}.{LANES[lane]}"


def lanes_reading(form, reg, a, b, negate):
    """What Xenia's model makes of a and b of one register, as the probe names it: a sum or product with the lower
    lane first, a difference in its order (xenia:src/xenia/gpu/shader_interpreter.cc:655-761)."""
    va, vb = (-temp_value(reg, a), -temp_value(reg, b)) if negate else (temp_value(reg, a), temp_value(reg, b))
    lo, hi = lane_name(reg, min(a, b)), lane_name(reg, max(a, b))
    if form == "muls":
        return f"{lo} * {hi}"
    if form == "adds":
        return f"{neg(negate)}{lo} {'-' if negate else '+'} {hi}"
    if form == "subs":
        first, second = (b, a) if negate else (a, b)
        return f"{lane_name(reg, first)} - {lane_name(reg, second)}"
    picked = a if (va >= vb if form == "maxs" else va < vb) else b
    return f"{neg(negate)}{lane_name(reg, picked)}"


def lanes_case(op, reg, a, b, negate, variant, exported=SCALAR_DEST):
    form, opcode, family = op
    return Case(form, family, "DOMAIN_LANES", lanes_form(opcode, reg, a, b, negate),
                f"{form} {neg(negate)}r{reg}.{LANES[a]}{LANES[b]}", variant, lanes_reading(form, reg, a, b, negate),
                exported)


def unary_case(op, reg, lane, negate, variant, exported=SCALAR_DEST):
    form, opcode, family = op
    operand = f"{neg(negate)}{lane_name(reg, lane)}"
    if form == "maxs copy":
        text, reading = f"maxs {neg(negate)}r{reg}.{LANES[lane] * 2}", operand
    else:
        text, reading = f"{form} {operand}", f"{form[:-1]}({operand})"
    return Case(form, family, "DOMAIN_LANES", lanes_form(opcode, reg, lane, lane, negate), text, variant, reading,
                exported)


def control_case(variant, exported=SCALAR_DEST):
    """rcp of c1.y, 2^-15, exact, which no input holds."""
    return Case("rcp, the control", "OP_RCP", "DOMAIN_ALL", lanes_form(S.kRcp, 1, Y, Y, is_temp=False), "rcp c1.y",
                variant, "rcp(c1.y)", exported)


def cases():
    """Every case once, in the order the probe runs them."""
    found = []
    for op in CONSTANT_OPS:
        found += [constant_case(op, 1, W, t, X, False, v) for t in TEMPS for v in VARIANTS]
        found += [constant_case(op, row, a, t, X, False, v) for row in NAMED for a in range(4) for t in (0, 1)
                  for v in CO_ISSUED]
        found += [constant_case(op, 1, W, t, b, False, v) for t in (0, 1, 2, 5, 9, 17) for b in range(4)
                  for v in CO_ISSUED]
        found += [constant_case(op, row, a, t, X, True, v) for t in (0, 1, 2, 9) for row, a in ((1, W), (13, X))
                  for v in CO_ISSUED]
        found += [constant_case(op, 1, W, 9, X, False, v, VECTOR_DEST) for v in VARIANTS if v != "alone"]
    for op in LANE_OPS:
        pairs = [(a, b) for a, b in itertools.product(range(4), repeat=2) if op[0] != "subs" or a != b]
        found += [lanes_case(op, ODD_REGISTER, a, b, False, "add") for a, b in pairs]
        found += [lanes_case(op, EVEN_REGISTER, a, b, False, "add") for a, b in FOUR_PAIRS]
        found += [lanes_case(op, ODD_REGISTER, a, b, True, "add") for a, b in FOUR_PAIRS]
        found += [lanes_case(op, ODD_REGISTER, a, b, False, "alone") for a, b in FOUR_PAIRS]
        found += [lanes_case(op, ODD_REGISTER, a, b, False, "mul c2") for a, b in FOUR_PAIRS[:2]]
        found.append(lanes_case(op, ODD_REGISTER, W, X, False, "add", VECTOR_DEST))
    for op in UNARY_OPS:
        found += [unary_case(op, ODD_REGISTER, lane, False, "add") for lane in range(4)]
        # frc(-x) is frc of another lane's 1 - eighths, so negated frcs name no lane; floors and truncs show negation.
        found += [unary_case(op, ODD_REGISTER, lane, True, "add") for lane in range(4) if op[0] != "frcs"]
        found += [unary_case(op, EVEN_REGISTER, lane, False, "add") for lane in range(4)]
        found += [unary_case(op, ODD_REGISTER, W, False, v) for v in ("alone", "mul c2")]
        found.append(unary_case(op, ODD_REGISTER, W, False, "add", VECTOR_DEST))
    found += [control_case(v) for v in CO_ISSUED] + [control_case("add", VECTOR_DEST)]
    unique, seen = [], set()
    for case in found:
        if case.name() not in seen:
            seen.add(case.name())
            unique.append(case)
    return unique


LOADS = [enc.build_alu(vector_dest=k, vector_write_mask=0xF, src1_reg=TEMP_ROW + k, src1_is_temp=False,
                       src2_reg=TEMP_ROW + k, src2_is_temp=False) for k in range(REGISTERS)]


def pixel_shader(instruction, exported):
    """alloc colors, then the loads in execs of EXEC_MOST, the instruction in an exec of its own and the export."""
    execs = [LOADS[i:i + EXEC_MOST] for i in range(0, REGISTERS, EXEC_MOST)]
    execs += [[instruction], [tri.export(tri.EXPORT_COLOR_0, exported)]]
    control_flow = [enc.build_cf_alloc(alloc_type=tri.ALLOC_COLORS)]
    address = (len(execs) + 2) // 2
    for n, block in enumerate(execs):
        control_flow.append(tri.execute(address, len(block), end=n == len(execs) - 1))
        address += len(block)
    dwords = tri.pack(control_flow, [w for block in execs for w in block])
    blob = ce.build_container(
        "pixel", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(ps_num_reg=REGISTERS - 1, ps_export_mode=tri.PS_EXPORT_MODE_COLOR),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)
    # The ucode ends the container; the instruction is its second-last slot and the export its last.
    assert blob.endswith(struct.pack(f">{len(dwords)}I", *dwords))
    return list(struct.unpack(f">{len(blob) // 4}I", blob))


def same(x, y):
    return struct.pack(">f", x) == struct.pack(">f", y)


UNARY = {"OP_FLOOR": simulate._floor, "OP_FRC": lambda x: x - simulate._floor(x), "OP_TRUNC": simulate._trunc,
         "OP_RCP": lambda x: 1.0 / x}


def enumerated(op, a_set, b_set, named):
    """Each reading the probe tries for op over a domain, in its order, with its value before rounding: a sum or
    product once for both orders where both operands come from one set."""
    if op in UNARY:
        word = op[3:].lower()
        for i in a_set:
            name, v = named[i]
            yield f"{word}({name})", UNARY[op](v)
            # frc(-x) is frc of another lane's 1 - eighths, so the probe doesn't try it.
            if op != "OP_FRC":
                yield f"{word}(-{name})", UNARY[op](-v)
        return
    if op not in ("OP_MUL", "OP_ADD"):
        return
    shared = a_set == b_set
    for i in a_set:
        for j in b_set:
            (a, va), (b, vb) = named[i], named[j]
            once = not shared or j >= i
            if op == "OP_MUL":
                if once:
                    yield f"{a} * {b}", va * vb
                    yield f"-({a} * {b})", -va * vb
                continue
            if once:
                yield f"{a} + {b}", va + vb
                yield f"-{a} - {b}", -va - vb
            if not shared or j != i:
                yield f"{a} - {b}", va - vb
            if not shared:
                yield f"-{a} + {b}", -va + vb


def domain_sets(domain, reg, named):
    temps, everything = range(4 * REGISTERS), range(len(named))
    return {"DOMAIN_CONSTANT_TEMP": (range(4 * REGISTERS, len(named)), temps),
            "DOMAIN_LANES": (range(4 * reg, 4 * reg + 4),) * 2, "DOMAIN_ALL": (everything, everything)}[domain]


def readings(value, op, domain, reg, named):
    """The probe's search, as tests/probe_scalar_forms.c runs it: the clear, each input or its negation, the op over the
    case's domain, then over every input if nothing matched."""
    found = ["the clear"] if same(value, CLEAR) else []
    for name, v in named:
        found += [name] if same(value, v) else []
        found += [f"-{name}"] if same(value, -v) else []
    found += [text for text, v in enumerated(op, *domain_sets(domain, reg, named), named)
              if same(value, simulate.single(v))]
    if not found and domain != "DOMAIN_ALL":
        found = [text for text, v in enumerated(op, *domain_sets("DOMAIN_ALL", reg, named), named)
                 if same(value, simulate.single(v))]
    return found


def check_domain(op, domain, reg, named):
    """Stops unless every reading of the domain is exact, and no two of them, nor one and an input, give one value."""
    seen = {}
    for name, v in named:
        seen[v], seen[-v] = name, f"-{name}"
    for text, v in enumerated(op, *domain_sets(domain, reg, named), named):
        if simulate.single(v) != v:
            sys.exit(f"gen_scalar_forms_probe: {op} over {domain} r{reg}: {text} isn't exact")
        if v in seen:
            sys.exit(f"gen_scalar_forms_probe: {op} over {domain} r{reg}: {text} and {seen[v]} give one value")
        seen[v] = text


def check_domains(named):
    for op in ("OP_MUL", "OP_ADD"):
        check_domain(op, "DOMAIN_CONSTANT_TEMP", 0, named)
    for reg in (EVEN_REGISTER, ODD_REGISTER):
        for op in ("OP_MUL", "OP_ADD", "OP_FLOOR", "OP_FRC", "OP_TRUNC"):
            check_domain(op, "DOMAIN_LANES", reg, named)


def check_case(case, words, named, rows):
    """Stops unless the disassembler reads the instruction as the case names it, and Xenia's model runs the case to a
    value only the case's reading gives, or for a vector half one its reading gives; returns that value."""
    text = xu.disassemble(tri.pack([tri.execute(1, 1, end=True)], [tuple(words[-6:-3])]), "pixel")
    lines = [line.split("*/")[-1].strip().lstrip("+").strip() for line in text.splitlines()[1:-1]]
    op, operands = case.text.split(" ", 1)
    if lines != [VARIANTS[case.variant][1], f"{op} r61.x___, {operands}"]:
        sys.exit(f"gen_scalar_forms_probe: {case.name()}: the disassembler reads {lines}")
    blob = struct.pack(f">{len(words)}I", *words)
    value = simulate.run(blob, {}, rows).exports[tri.EXPORT_COLOR_0][0]
    if case.alike is not None:
        if not same(value, case.alike):
            sys.exit(f"gen_scalar_forms_probe: {case.name()}: the model gives {value!r}, not {case.reading}")
        return value
    found = readings(value, case.op, case.domain, case.fields["src3_reg"] & 0x3F, named)
    if found != [case.reading] and not (case.form == VECTOR_HALF and case.reading in found):
        sys.exit(f"gen_scalar_forms_probe: {case.name()}: Xenia's model gives {value!r}, read as {found}, "
                 f"not {case.reading}")
    return value


def c_float(value):
    return f"{value!r}f" if value != int(value) else f"{value:.1f}f"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    named, rows = inputs(), constants()
    check_domains(named)
    all_cases = cases()
    template = pixel_shader(all_cases[0].instruction(), SCALAR_DEST)
    lines = ["// Generated by tools/gen_scalar_forms_probe.py; don't edit.", "",
             f"enum {{ PROBE_REGISTERS = {REGISTERS}, PROBE_TEMP_ROW = {TEMP_ROW}, PROBE_CONSTANT_ROWS = "
             f"{CONSTANT_ROWS}, PROBE_NAMED_CONSTANTS = {len(EXPONENTS)}, PROBE_TEMPLATE_WORDS = {len(template)}, "
             f"PROBE_INSTRUCTION_WORD = {len(template) - 6}, PROBE_EXPORT_WORD = {len(template) - 3} }};", "",
             f"static const float probe_clear = {c_float(CLEAR)};", "",
             "static const uint8_t probe_named_constants[PROBE_NAMED_CONSTANTS] = { " +
             ", ".join(str(r) for r in sorted(EXPONENTS)) + " };", "",
             "static const float probe_constants[PROBE_CONSTANT_ROWS][4] = {"]
    lines += ["\t{ " + ", ".join(c_float(v) for v in rows[n]) + " }," for n in range(CONSTANT_ROWS)] + ["};", ""]
    lines += ["static const char *const probe_form_names[] = { " + ", ".join(f'"{f}"' for f in FORMS) + " };", ""]
    lines += ["static const uint32_t probe_template[PROBE_TEMPLATE_WORDS] = {", pack.word_lines(template), "};", ""]
    table = []
    for case in all_cases:
        words = pixel_shader(case.instruction(), case.exported)
        export = list(tri.export(tri.EXPORT_COLOR_0, case.exported))
        patched = template[:-6] + list(case.instruction()) + export
        if patched != words:
            sys.exit(f"gen_scalar_forms_probe: {case.name()}: patching the template doesn't give its container")
        value = check_case(case, words, named, rows)
        instruction = ", ".join(f"0x{w:08X}" for w in case.instruction())
        table.append(f'\t{{ "{case.name()}", "{case.reading}", {FORMS.index(case.form)}, {case.op}, {case.domain}, '
                     f"{{ {instruction} }}, {{ {', '.join(f'0x{w:08X}' for w in export)} }}, {c_float(value)} }},")
    lines += ["static const ProbeCase probe_cases[] = {"] + table + ["};", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
