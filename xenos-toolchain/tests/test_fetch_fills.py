#!/usr/bin/env python3
"""With the optimiser on, a vector built from a vertex input's own components, each in its lane, and 0 or 1 in lanes
past the input's components is the input itself: its fetch's destination swizzle writes the 0 or 1
(xenia:src/xenia/gpu/ucode.h:677-692; xsdk:src/xsc_lower.c, fetched_construct), which xsdk's patch of the fetch for
the app's layout keeps (xsdk:src/shader.c, picked_swizzle, rewrites only the selectors of x to w).

- vk_fetch_fills.vert, optimiser on: in_pos, a vec3, fetched .xyz1 for vec4(in_pos, 1.0); in_uv, a vec2, .xy01 for
  vec4(in_uv, 0.0, 1.0); in_nrm, a vec3, .xyz0 for vec4(in_nrm, 0.0), its vec4(in_nrm, 1.0) left to the ALU; in_color,
  a vec4, .xyzw, as vec4(in_color.rgb, 1.0) fills a component it has. Fetches go in reverse Location order.
- Optimiser off, every fetch is .xyzw, as before.
- Both programs give the same exports on random inputs and constants (xdis/equivalence.py, as tools/check_optimize.py
  compares them), the optimised one in 10 ALU instructions, where it took 17 without the fills from the fetches.
- A fill turned to the other constant, in_pos's w made 0, shows as a difference.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import check_optimize  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.spirv as spirv  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_xsc_lower import TD  # noqa: E402

NAME = "vk_fetch_fills.vert"
# in_nrm, in_uv, in_color, in_pos: the fetches in program order.
WANT = ["xyz0", "xy01", "xyzw", "xyz1"]
# Without fills from the fetches the optimiser left 17 (a copy of the fetched part and one of the constant each).
WANT_ALU = 10
SELECTORS = "xyzw01?_"


def fetches(blob):
    """Each vertex fetch's (destination swizzle as text, its three words)."""
    c = container.Container(blob)
    out = []
    for rec in xu._decode_program(c.ucode_dwords, c.shader_type)[0]:
        if rec.kind == "vfetch":
            swizzle = xu.VertexFetchInstruction(*rec.raw).dest_swizzle()
            out.append(("".join(SELECTORS[swizzle >> (3 * i) & 7] for i in range(4)), tuple(rec.raw)))
    return out


def with_swizzle(blob, words, lanes):
    """The container with that fetch's destination swizzle set to lanes."""
    c = container.Container(blob)
    at = next(i for i in range(len(c.ucode_dwords) - 2) if tuple(c.ucode_dwords[i:i + 3]) == words)
    swizzle = sum(SELECTORS.index(ch) << (3 * i) for i, ch in enumerate(lanes))
    offset = c.offset + c.sh_off + 4 * (at + 1)
    return blob[:offset] + struct.pack(">I", (words[1] & ~0xFFF) | swizzle) + blob[offset + 4:]


def check(name, ok, detail):
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")
    return ok


def main():
    blob = (TD / f"{NAME}.spv").read_bytes()
    module = spirv.Module.parse(blob)
    plain, manifest = lower_ex(spirv.Module.parse(blob), XSDK_REGISTERS)
    optimized, manifest_opt = lower_ex(spirv.Module.parse(blob), XSDK_REGISTERS, optimize=True)
    on = fetches(optimized)
    widths = check_optimize.output_widths(module)
    problem = check_optimize.compare(plain, optimized, manifest, manifest_opt, widths, 100, 1)
    alu = check_optimize.counts(optimized)["alu"]
    xyz1 = next(words for lanes, words in on if lanes == "xyz1")
    wrong = check_optimize.compare(plain, with_swizzle(optimized, xyz1, "xyz0"), manifest, manifest_opt, widths, 8, 1)
    results = [
        check("fills from the fetches", [lanes for lanes, _ in on] == WANT,
              f"in_nrm, in_uv, in_color, in_pos fetched {', '.join(lanes for lanes, _ in on)}"),
        check("optimiser off", [lanes for lanes, _ in fetches(plain)] == ["xyzw"] * 4,
              "every fetch " + ", ".join(lanes for lanes, _ in fetches(plain))),
        check("same exports", problem is None, problem or f"100 trials, {alu} ALU instructions"),
        check("ALU", alu == WANT_ALU, f"{alu}, want {WANT_ALU}"),
        check("a wrong fill shows", wrong is not None, wrong or "nothing seen"),
    ]
    if not all(results):
        raise SystemExit(1)
    print("OK")


if __name__ == "__main__":
    main()
