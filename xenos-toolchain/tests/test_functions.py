#!/usr/bin/env python3
"""xsc inlines GLSL's helper functions, which glslang leaves as calls (xsdk:src/xsc_inline.c).

xost took no OpFunctionCall, and the vertex lowering refused the parameters' copies first: RetroArch's XMB ribbon,
whose noise is three helpers deep, came out "a store into part of local".

- vk_functions.vert calls one helper three times, one from another, with an inout and an out parameter and a store
  into part of a parameter; the simulator gives the position the GLSL says.
- vk_functions.frag inlines a helper of a helper into a pixel shader, each called with the other's result.
- vk_function_branch.frag's helper returns early, control flow the inliner doesn't take: refused by name.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import OPOS, UV, check, module, push_constants, run_fragment  # noqa: E402

A = [1.5, -2.0, 0.75, 3.0]
B = [0.5, 1.25, -1.0, 0.125]
POSITION = [3.0, -2.0, 0.5, 1.0]


def wave(x):
    return x * 0.5 + 0.25


def twice(v, k):
    v = [v[0], wave(v[1]) * k]
    return [v[0] + wave(v[0]), v[1] + wave(v[0])]


def vertex_want():
    p = list(POSITION)
    p[0] *= A[0]
    p[2] *= A[0]
    total = p[0] + p[2]
    first, second = twice(p[:2], B[1]), twice(A[2:], total)
    return [first[0] + second[0], first[1] + second[1], p[2], wave(B[3])]


def mix(x, y, t):
    return [a + (b - a) * t for a, b in zip(x, y)]


def main():
    blob, manifest = lower_ex(module("vk_functions.vert"), XSDK_REGISTERS)
    constants = push_constants("vk_functions.vert", [A, B])
    constants.update({r: list(v) for r, v in manifest})
    result = simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                          vertex_fetch=lambda n, fetch, index: POSITION)
    check("vk_functions.vert, three helpers inlined", result.exports[OPOS], vertex_want())

    shaded = mix(A, B, UV[0])[:3] + [UV[1]]
    check("vk_functions.frag, a helper of a helper", run_fragment("vk_functions.frag", [A, B]), mix(shaded, A, 0.25))

    try:
        lower_ex(module("vk_function_branch.frag"), XSDK_REGISTERS)
    except LowerError as error:
        if "a function with control flow" not in str(error):
            raise SystemExit(f"FAILED: vk_function_branch.frag refused for another reason: {error}")
        print(f"  PASS vk_function_branch.frag refused: {error}")
    else:
        raise SystemExit("FAILED: vk_function_branch.frag compiled")
    print("OK")


if __name__ == "__main__":
    main()
