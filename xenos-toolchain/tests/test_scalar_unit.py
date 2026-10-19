#!/usr/bin/env python3
"""Ops that write one value co-issued in the scalar unit beside vector ops (xsdk:src/xsc_optimize.c, scalar_form).

vk_scalar_unit.frag computes each kind beside a dot product, which has no scalar form: with its operands in one register
an add, a subtraction, an add of a negation, a multiply, a square, a max and a min, and a floor and a fraction of a sum
(of an input alone, copy propagation would leave the floor's unread second operand naming a copy that goes, which the
allocator takes for an input); with a constant and a temp, an input or a sum, a multiply, an add, a subtraction and a
subtraction of the constant. It keeps in the vector unit a multiply of two registers, one of two operands negated, a
max with a constant, one of two absolute, and a dot product with a literal 1 in lane x, which dp2add would add last,
rounding differently. Each runs on xdis/simulate.py, the optimiser off and on, at inputs with NaN and -0 where a max's
or a min's operands' order shows and at ordinary ones, and every export must be the same bit for bit, a NaN as NaN. With
the optimiser on the program must hold exactly the scalar ops expected, each co-issued, the vector ops it keeps, a dp3
and no dp2add, and the ALU instructions they leave.

mulsc, addsc and subsc spread their temp's index over the opcode, src3_sel and src3's swizzle (xsdk:src/xsc_program.c,
encode). Built by hand reading r45, r18 and r33, which use every bit of an index, each must give on the simulator what
its constant and temp make.
"""
import collections
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
from test_local_stores import module  # noqa: E402
from test_optimize import build, export, r, scalar  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

S = xu.AluScalarOpcode
NAME = "vk_scalar_unit.frag"
NAN = math.nan
# (v_a, v_b): NaN in the max's and the min's first operands, -0 against +0 in them, and ordinary values.
INPUTS = [([NAN, NAN, 2.0, -3.0], [1.5, -0.4, 0.7, 2.0]), ([0.0, 0.0, -0.0, -0.0], [1.5, -0.4, 0.7, 2.0]),
          ([0.3, -1.7, 2.25, 0.6], [1.5, -0.4, 0.7, 2.0])]
# Each scalar op by name, with its operand's negation: three of the maxs are the copies that build the dp3's vector.
SCALAR = {("adds", False): 1, ("subs", False): 1, ("subs", True): 1, ("muls", False): 2, ("maxs", False): 4,
          ("mins", False): 1, ("floors", False): 1, ("frcs", False): 1, ("mulsc", False): 2, ("addsc", False): 2,
          ("subsc", False): 2, ("subsc", True): 2}
# The ops kept in the vector unit, one lane each, but the dot products: two muls, a max with a constant and the
# absolute value's max of x and -x, the absolute value's add and the two sums.
KEPT = {"mul": 2, "max": 2, "add": 3}
ALU = {False: 157, True: 38}
# By hand: (opcode, temp, its component, constant, its component, negated), each writing its own lane of r2.
BY_HAND = [(S.kMulsc0, 45, 0, 7, 2, False), (S.kAddsc0, 18, 1, 9, 0, False), (S.kSubsc0, 33, 3, 7, 3, True)]
TEMP = [0.375, -2.5, 1.25, 7.5]
CONSTANT = [3.0, -0.5, 1.75, 0.625]


def census(blob):
    """(ALU instructions, co-issued, scalar ops by (name, negated), one-lane vector ops by name, vector op names)."""
    c = container.Container(blob)
    alu = co_issued = 0
    scalar_ops, one_lane, names = collections.Counter(), collections.Counter(), set()
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind != "alu":
            continue
        a, d = xu.AluInstruction(*rec.raw), rec.detail
        alu += 1
        vector, scalar_op = not d.IsVectorOpDefaultNop(), not d.IsScalarOpDefaultNop()
        co_issued += vector and scalar_op
        if scalar_op:
            scalar_ops[(xu.kAluScalarOpcodeInfos[a.scalar_opcode_raw()][0], a.src_negate(3))] += 1
        if vector:
            names.add(d.vector_opcode_name)
            if bin(a.GetVectorOpResultWriteMask()).count("1") == 1 and not a.is_export():
                one_lane[d.vector_opcode_name] += 1
    return alu, co_issued, scalar_ops, one_lane, names


def bits(exports):
    """Each export's bits, a NaN as NaN whatever its sign."""
    return {reg: [b"NaN" if math.isnan(v) else struct.pack(">f", v) for v in values] for reg, values in exports.items()}


def by_hand():
    """mulsc, addsc and subsc built by hand: src3 their temp, scalar_constant their constant (xsc_private.h)."""
    body, want = [], []
    for lane, (op, temp, t, constant, c, negate) in enumerate(BY_HAND):
        f = scalar(op, 2, r(temp, negate=negate), 1 << lane)
        f["src3_swizzle"] = t | ((c - 3) & 3) << 6
        f["scalar_constant"] = constant
        body.append(f)
        a, b = (-CONSTANT[c], -TEMP[t]) if negate else (CONSTANT[c], TEMP[t])
        want.append(simulate.single(a * b if op == S.kMulsc0 else a + b if op == S.kAddsc0 else a - b))
    blob = build(body, [export(r(2))], False)
    got = simulate.run(blob, {temp: TEMP for _, temp, *_ in BY_HAND}, {7: CONSTANT, 9: CONSTANT}).exports[0][:3]
    if got != want:
        raise SystemExit(f"  FAIL mulsc, addsc and subsc of r45, r18 and r33 by hand: {got}, want {want}")
    print(f"  PASS mulsc, addsc and subsc of r45, r18 and r33 by hand: {got}")


def main():
    runs = {}
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
        runs[optimize] = [bits(simulate.run(blob, {0: a, 1: b}, literals).exports) for a, b in INPUTS]
        alu, co_issued, scalar_ops, one_lane, names = census(blob)
        kept = {name: n for name, n in one_lane.items() if name not in ("dp4", "dp3")}
        want = (ALU[optimize], sum(SCALAR.values()) if optimize else 0)
        if (alu, co_issued) != want or scalar_ops != (collections.Counter(SCALAR) if optimize else {}):
            raise SystemExit(f"  FAIL {NAME}, {tag}: {alu} ALU, {co_issued} co-issued, scalar ops {dict(scalar_ops)}; "
                             f"want {want[0]}, {want[1]} and {SCALAR if optimize else {}}")
        if optimize and (kept != KEPT or "dp3" not in names or "dp2add" in names):
            raise SystemExit(f"  FAIL {NAME}, {tag}: vector ops kept {kept}, {sorted(names)}; want {KEPT}, a dp3")
        print(f"  PASS {NAME}, {tag}: {alu} ALU instructions, {co_issued} co-issued, {sum(scalar_ops.values())} "
              f"scalar ops")
    if runs[True] != runs[False]:
        raise SystemExit(f"  FAIL {NAME}: exports differ, optimiser off {runs[False]}, on {runs[True]}")
    print(f"  PASS {NAME}: exports the same bit for bit at {len(INPUTS)} inputs, NaN and -0 among them")
    by_hand()
    print("OK")


if __name__ == "__main__":
    main()
