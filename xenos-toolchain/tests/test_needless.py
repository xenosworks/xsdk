#!/usr/bin/env python3
"""tools/needless.py's counts on a hand-built pixel program, the C building it as tests/test_optimize.py's cases are
built, optimiser off, so each kind is where the program puts it:
  - x + 0 and a mix whose weight is 0, a mad by 0: two identities and a mix pair;
  - a ±4 clamp of a saturated product, then a wrap's floor of it times 255 divided by 256, through an rcp of 256 and a
    one-lane mul: two clamps that can't change their value, a floor its range fixes at 0, the mad that subtracts it,
    and the mul only that floor reads, 5 range-useless; a lane divided by a constant and its rcp;
  - four one-lane ops alone in their slots: the mul, the floor, the mad and an add.
The same program without the saturate has no range-useless op and the rest the same. Runs in WSL, where make builds
build/host/libxsc.so.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import needless  # noqa: E402
from test_optimize import S, V, alu, build, c, export, r, scalar  # noqa: E402

LITERALS = [(5, (0.0,) * 4), (7, (-4.0,) * 4), (8, (4.0,) * 4), (9, (255.0,) * 4), (10, (256.0,) * 4)]


def body(saturate):
    return [alu(V.kAdd, 2, 0xF, r(0), c(5)),
            alu(V.kAdd, 3, 0xF, r(0), r(1, negate=True)),
            alu(V.kMad, 4, 0xF, r(3), c(5), r(1)),
            alu(V.kMul, 5, 0xF, r(0), r(1), clamp=saturate),
            alu(V.kMax, 6, 0xF, r(5), c(7)),
            alu(V.kMin, 7, 0xF, r(6), c(8)),
            alu(V.kMul, 8, 0xF, r(7), c(9)),
            scalar(S.kRcp, 9, c(10), 0x1),
            alu(V.kMul, 10, 0x1, r(8), r(9, "xxxx")),
            alu(V.kFloor, 11, 0x1, r(10)),
            alu(V.kMad, 12, 0x1, r(11, negate=True), c(10), r(8)),
            alu(V.kAdd, 13, 0xF, r(2), r(4)),
            alu(V.kAdd, 14, 0x1, r(0), r(1)),
            alu(V.kAdd, 15, 0xF, r(13), r(12, "xxxx")),
            alu(V.kAdd, 16, 0xF, r(15), r(14, "xxxx"))]


def case(name, saturate, want):
    got = needless.count(build(body(saturate), [export(r(16))], False), LITERALS)
    got = {k: v for k, v in got.items() if k != "kinds"}
    ok = got == want
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {got}" + ("" if ok else f", {want} expected"))
    return ok


def main():
    counts = {"identities": 2, "mix_pairs": 1, "divided_lanes": 1, "constant_rcps": 1, "single_lane": 4,
              "range_useless": 5, "without_flow": 0}
    ok = case("each kind counted", True, counts) & case("no range without the saturate", False,
                                                        {**counts, "range_useless": 0})
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
