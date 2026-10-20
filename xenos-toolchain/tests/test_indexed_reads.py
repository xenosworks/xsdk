#!/usr/bin/env python3
"""Constants read at a run-time index, through the whole optimiser (xsdk:src/xsc_optimize.c): a copy of a constant
read at a0 goes into its readers, and products and exports take it, but only where a0 still holds its index, as no a0
load or alloc lies between the copy and the reader (same_values; docs/HARDWARE.md, "a0 doesn't survive an alloc").

vk_indexed_reads.vert reads offsets[j] at j, a matrix's columns at 4j, tints[i] at i, then past the interpolators'
alloc scales[j] and offsets[i]; it uses offsets[j] past i's load and tints[i] past the alloc, before any load. A group
of 64 vertices, i 0 to 15 and j 0 to 3, each its own a0, runs on xdis/simulate.py, the optimiser off and on: each
vertex's exports must be what the GLSL gives exactly, its rows and inputs dyadic so nothing rounds. simulate.py loses
a0 at an alloc, as the console does, and raises at a read at it with no load since, which a program built by hand
shows. The optimised program must read at a0 with a load since every alloc, keep the two copies the rules keep and
take the ALU instructions and copies below.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import xdis.equivalence as equivalence  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import test_optimize as hand  # noqa: E402
from check_optimize import counts  # noqa: E402
from test_dynamic_index import a0_reloads  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402

NAME = "vk_indexed_reads.vert"
OPOS = int(xu.ExportRegister.kVSPosition)
I_COUNT, J_COUNT = 16, 4
# The ALU instructions and copies off and on: the copies left on are offsets[j]'s and tints[i]'s.
ALU = {False: 30, True: 17}
COPIES = {False: 14, True: 3}


def rows():
    """The block's registers: m[4] by columns, tints[16], offsets[16], scales[16]."""
    m = [[[float(j - 2 + c - r) for r in range(4)] for c in range(4)] for j in range(J_COUNT)]
    tints = [[i / 4.0, 1.0 - i / 8.0, 0.5, float(i)] for i in range(I_COUNT)]
    offsets = [[n / 2.0, -n / 4.0, 1.0, 2.0] for n in range(I_COUNT)]
    scales = [[2.0, float(n), 0.25, -1.0] for n in range(I_COUNT)]
    return m, tints, offsets, scales


def vertex(k):
    """Vertex k's attribute: x, y, i, j."""
    i, j = k % I_COUNT, k // I_COUNT
    return [0.5 + 0.25 * i, -1.0 + 0.125 * j, float(i), float(j)]


def wants(k):
    m, tints, offsets, scales = rows()
    x, y, i, j = vertex(k)
    i, j = int(i), int(j)
    p = [x, y, 0.5, 1.0]
    pos = [sum(m[j][c][r] * p[c] for c in range(4)) for r in range(4)]
    position = [pos[n] + offsets[j][n] * tints[i][n] for n in range(4)]
    color = [tints[i][n] * 2.0 + scales[j][n] * offsets[j][n] for n in range(4)]
    return {OPOS: position, 0: color, 1: offsets[i]}


def run(optimize):
    """The group's exports and the program."""
    blob, manifest = lower_ex(module(NAME), XSDK_REGISTERS, optimize)
    base = next(b for b in resolve_interface(module(NAME)).blocks if b.kind == "uniform").reg
    m, tints, offsets, scales = rows()
    flat = [col for matrix in m for col in matrix] + tints + offsets + scales
    constants = {base + n: list(row) for n, row in enumerate(flat)}
    constants.update({reg: list(values) for reg, values in manifest})
    lanes = [{0: [float(k), 0.0, 0.0, 0.0]} for k in range(I_COUNT * J_COUNT)]
    results = simulate.run_group(blob, lanes, constants, lambda n, fetch, index: vertex(int(index)))
    return [r.exports for r in results], blob


def check_program(optimize):
    tag = f"optimiser {'on' if optimize else 'off'}"
    exports, blob = run(optimize)
    for k, got in enumerate(exports):
        want = wants(k)
        if any(got.get(reg) != values for reg, values in want.items()):
            raise SystemExit(f"  FAIL {NAME}, {tag}: vertex {k}, i {k % I_COUNT}, j {k // I_COUNT}: exports {got}, "
                             f"want {want}")
    reloads, problem = a0_reloads(blob)
    found = {"ALU": counts(blob)["alu"], "copies": counts(blob)["copies"], "a0 reloads": reloads}
    want = {"ALU": ALU[optimize], "copies": COPIES[optimize], "a0 reloads": 0}
    if problem or found != want:
        raise SystemExit(f"  FAIL {NAME}, {tag}: {found}, not {want}{f'; {problem}' if problem else ''}")
    print(f"  PASS {NAME}, {tag}: 64 vertices' exports exact, each its own i and j; {found}")


def read_past_alloc(reload):
    """A pixel program built by hand: a0 loaded, then the colours' alloc and an export of c[4+a0], a0 loaded again
    before it if reload."""
    read = dict(hand.export(hand.c(4)), is_const_address_register_relative=True, src1_const_rel=True,
                src2_const_rel=True)
    exports = ([hand.set_a0(hand.r(0, "xxxx"))] if reload else []) + [read]
    return hand.build([hand.set_a0(hand.r(0, "xxxx"))], exports, False)


def check_simulator():
    constants = {n: [float(n)] * 4 for n in range(16)}
    registers = {0: [3.0] * 4, 1: [0.0] * 4}
    try:
        simulate.run(read_past_alloc(False), registers, constants)
    except RuntimeError as e:
        lost = str(e)
    else:
        raise SystemExit("  FAIL the simulator: a read at a0 past an alloc with no load since ran")
    if equivalence.run(read_past_alloc(False), registers, constants)[0] != [None] * 4:
        raise SystemExit("  FAIL xdis/equivalence.py: a read at a0 past an alloc with no load since is defined")
    got = simulate.run(read_past_alloc(True), registers, constants).exports[0]
    if got != [7.0] * 4 or equivalence.run(read_past_alloc(True), registers, constants)[0] != [7.0] * 4:
        raise SystemExit(f"  FAIL the simulator: a read at a0 loaded again past the alloc gave {got}, not c7")
    print(f"  PASS the simulators: a read at a0 past an alloc raises ({lost}) and is undefined, and with a0 loaded "
          f"again reads c[4 + 3]")


def check_unread_operand():
    """vk_dynamic_rows.vert truncates a copy of its index with a trunc that names it in operand 2 too, which it doesn't
    read. The copy goes into operand 1, and operand 2 must follow, or it names a temp nothing writes once the copy
    goes, which the allocator keeps as an input, 9 registers on."""
    name = "vk_dynamic_rows.vert"
    found = {o: counts(lower_ex(module(name), XSDK_REGISTERS, o)[0])["registers"] for o in (False, True)}
    if found != {False: 6, True: 2}:
        raise SystemExit(f"  FAIL {name}: registers off and on {found}, not 6 and 2")
    print(f"  PASS {name}: 6 registers off, 2 on")


def main():
    check_simulator()
    for optimize in (False, True):
        check_program(optimize)
    check_unread_operand()
    print("OK")


if __name__ == "__main__":
    main()
