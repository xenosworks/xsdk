"""A GLSL pixel shader's inputs may skip a location, as PPSSPP's do (milestone 66).

xsc/testdata/input_gaps.frag reads locations 1, 3 and 0. Each must take the register of its own location, as the vertex
shader exports to that location's interpolator (src/xsc_program.c, exported_interpolators), with register 2 held unread.
xost refused it with "declare inputs in r0,r1,... order" before. Runs in WSL, as the other lowering tests do.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402

TD = ROOT / "xsc" / "testdata"
REGISTERS = 64
# Each location's own value, and 100 in the skipped one, which a misplaced read would add in.
INTERPOLATED = {0: [1.0, 0.0, 0.0, 0.0], 1: [0.0, 1.0, 0.0, 0.0], 2: [100.0] * 4, 3: [0.0, 0.0, 1.0, 0.0]}
EXPECTED = [1.0, 2.0, 4.0, 0.0]


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def main():
    spirv = (TD / "input_gaps.frag.spv").read_bytes()
    try:
        blob, manifest, _ = native.compile_spirv(spirv, REGISTERS, REGISTERS, True)
    except native.CompileError as e:
        fail(f"input_gaps.frag refused: {e}")
    records, _ = native.records(blob)
    registers = [r >> 8 & 0xF for r in records]
    if registers != [0, 1, 2, 3]:
        fail(f"input_gaps.frag: interpolator registers {registers}, not 0 to 3")
    constants = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
    got = simulate.run(blob, INTERPOLATED, constants).exports.get(0)
    if got != EXPECTED:
        fail(f"input_gaps.frag: {got}, not {EXPECTED}")
    print("  PASS input_gaps.frag: locations 1, 3 and 0 read their own registers, 2 held unread")


if __name__ == "__main__":
    main()
