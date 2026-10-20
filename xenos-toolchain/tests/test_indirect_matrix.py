#!/usr/bin/env python3
"""Common subexpressions through moves (xsdk:src/xsc_optimize.c, same_computation and value_of): two ops are one where
their operands hold the same values lane by lane, though moves of the same constants put them in two temps.

vk_indirect_matrix.frag builds an indirect texture matrix's rows twice from one uniform, as Dusklight's
1510c05add37f2b3.frag does for two indirect stages, and takes each row's dot product with one biased texel. It runs on
xdis/simulate.py, the optimiser off and on: each export must be what the GLSL gives exactly, its inputs integers and
its matrix dyadic so nothing rounds. Off, the program takes four dot products; on, two, the second stage's rows
common with the first's, and the ALU instructions below.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
from test_local_stores import module  # noqa: E402
from test_optimize import alu_count  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402

NAME = "vk_indirect_matrix.frag"
M0 = [0.5, -0.25, 0.125, 0.75]
M1 = [-0.5, 0.25, 0.0625, 3.0]
# in_ind, a texel times 255, and in_uv, for each run.
INPUTS = [([200.0, 64.0, 255.0, 7.0], [0.25, 0.5, -0.75, 1.0]), ([0.0, 128.0, 31.0, 0.0], [-2.0, 0.125, 4.0, -0.5])]
# The dot products and ALU instructions with the optimiser off and on.
DOTS = {False: 4, True: 2}
ALU = {False: 32, True: 10}


def wants(ind, uv):
    biased = [v - 128.0 for v in ind[:3]]
    rows = ([M0[0], M0[2], M1[0]], [M0[1], M0[3], M1[1]])
    product = [sum(a * b for a, b in zip(row, biased)) * M1[2] for row in rows]
    return [uv[0] + product[0], uv[1] + product[1], uv[2] + product[0], uv[3] + product[1]]


def dots(blob):
    cont = container.Container(blob)
    return sum(rec.kind == "alu" and rec.detail.vector_opcode == xu.AluVectorOpcode.kDp3
               for rec in xu._decode_program(cont.ucode_dwords, cont.shader_type)[0])


def main():
    base = next(b for b in resolve_interface(module(NAME)).blocks if b.kind == "uniform").reg
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
        constants = {base: M0, base + 1: M1}
        constants.update({reg: list(values) for reg, values in manifest})
        for ind, uv in INPUTS:
            got = simulate.run(blob, {0: ind, 1: uv}, constants).exports[0]
            if got != wants(ind, uv):
                raise SystemExit(f"  FAIL {NAME}, {tag}: in_ind {ind}, in_uv {uv}: {got}, not {wants(ind, uv)}")
        found = {"dot products": dots(blob), "ALU": alu_count(blob)}
        want = {"dot products": DOTS[optimize], "ALU": ALU[optimize]}
        if found != want:
            raise SystemExit(f"  FAIL {NAME}, {tag}: {found}, not {want}")
        print(f"  PASS {NAME}, {tag}: exports exact, {found}")
    print("OK")


if __name__ == "__main__":
    main()
