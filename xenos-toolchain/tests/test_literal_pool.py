#!/usr/bin/env python3
"""Literals pooled by the 32-bit floats a register holds (xsdk:src/xsc_lower.c, xsdk_xsc_literal_reg).

The pool keyed a literal by its components rounded to 9 places, so 1e-10 and 0 shared whichever register came first:
vk_literal_pool.frag's max(v, 1e-10) and step(1e-10, v) beside max(v, 0.0) read one value for both, as 21 of
Dusklight's vertex shaders did normalising a normal. Each lane runs on xdis/simulate.py, the optimiser off and on, and
every export component must be the GLSL's float exactly, as a tolerance would pass 1e-10 for 0.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
from test_data_loops import lowered  # noqa: E402

NAME = "vk_literal_pool.frag"
VALUES = ([-1.0, -1.0, -1.0, 0.0], [2.0, 0.0, 3.0, 0.0], [0.5, 1.0, -0.0, 0.0])


def wants(v):
    tiny = simulate.single(1e-10)
    return [max(v[0], tiny), 0.0 if v[1] < tiny else 1.0, max(v[2], 0.0), 1.0]


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        blob, literals, _ = lowered(NAME, optimize)
        for v in VALUES:
            got = simulate.run(blob, {0: v}, literals).exports[0]
            if got != wants(v):
                raise SystemExit(f"  FAIL {NAME}, {tag}, input {v}: {got} where the GLSL gives {wants(v)}")
        print(f"  PASS {NAME}, {tag}: {len(VALUES)} inputs exact")
    print("OK")


if __name__ == "__main__":
    main()
