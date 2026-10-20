#!/usr/bin/env python3
"""An export's one value as a scalar op beside the vector op exporting its register's other lanes (xsdk:
src/xsc_optimize.c, co_issue and merge): an export's scalar op writes the vector op's export register
(xenia:src/xenia/gpu/ucode.h:2041-2059).

vk_packed_alpha.frag exports a colour and its alpha apart, as Dusklight's GX shaders do: the alpha compare reads the
alpha in every lane, so it stays in its temp, and each export copies one value of it into w. It runs on
xdis/simulate.py, the optimiser off and on, and each export must be what the GLSL gives exactly, its inputs dyadic so
nothing rounds. On, o_color's alpha is a scalar op beside its colour's multiply, and o_fogged's stays a copy of its
own: its colour's mad reads a third operand, which a scalar op beside it would share. vk_packed_uniform.frag exports
a clamped uniform's colour and alpha apart: on, one instruction that names no temp, which still takes one register.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from check_optimize import counts  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402

NAME, UNIFORM = "vk_packed_alpha.frag", "vk_packed_uniform.frag"
C = [0.5, 0.25, 0.75, 0.75]
D = [0.5, 2.0, 0.25, 1.0]
FOG = [1.0, 0.0, 0.5, 0.0]
TEVREG = [1.5, 0.25, -2.0, 0.5]
# ALU instructions, copies and export instructions with a scalar op, the optimiser off and on.
FOUND = {False: {"ALU": 17, "copies": 7, "scalar exports": 0}, True: {"ALU": 10, "copies": 1, "scalar exports": 1}}


def scalar_exports(blob):
    cont = container.Container(blob)
    return sum(rec.kind == "alu" and xu.AluInstruction(*rec.raw).is_export() and
               xu.AluInstruction(*rec.raw).GetScalarOpResultWriteMask() != 0
               for rec in xu._decode_program(cont.ucode_dwords, cont.shader_type)[0])


def constants_of(name, rows, manifest):
    base = next(b for b in resolve_interface(module(name)).blocks if b.kind == "uniform").reg
    constants = {base + n: row for n, row in enumerate(rows)}
    constants.update({reg: list(values) for reg, values in manifest})
    return constants


def check_alpha(optimize, tag):
    blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
    result = simulate.run(blob, {0: C, 1: D}, constants_of(NAME, [FOG], manifest))
    alpha = min(max(C[3] * D[3], 0.0), 1.0)
    want = {0: [C[n] * D[n] for n in range(3)] + [alpha],
            1: [C[n] + (FOG[n] - C[n]) * D[0] for n in range(3)] + [alpha]}
    got = {reg: result.exports.get(reg) for reg in want}
    if result.killed or got != want:
        raise SystemExit(f"  FAIL {NAME}, {tag}: exports {got}, killed {result.killed}, not {want}")
    found = {"ALU": counts(blob)["alu"], "copies": counts(blob)["copies"], "scalar exports": scalar_exports(blob)}
    if found != FOUND[optimize]:
        raise SystemExit(f"  FAIL {NAME}, {tag}: {found}, not {FOUND[optimize]}")
    print(f"  PASS {NAME}, {tag}: exports exact, {found}")


def check_uniform():
    blob, manifest = lower_ex(module(UNIFORM), XSDK_REGISTERS, True)
    got = simulate.run(blob, {}, constants_of(UNIFORM, [[0.0] * 4, TEVREG], manifest)).exports.get(0)
    want = [min(max(v, 0.0), 1.0) for v in TEVREG]
    found = {"ALU": counts(blob)["alu"], "registers": counts(blob)["registers"]}
    if got != want or found != {"ALU": 1, "registers": 1}:
        raise SystemExit(f"  FAIL {UNIFORM}, optimiser on: export {got}, {found}, not {want}, 1 ALU and 1 register")
    print(f"  PASS {UNIFORM}, optimiser on: export exact, {found}")


def main():
    for optimize in (False, True):
        check_alpha(optimize, f"optimiser {'on' if optimize else 'off'}")
    check_uniform()
    print("OK")


if __name__ == "__main__":
    main()
