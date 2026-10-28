#!/usr/bin/env python3
"""xsc lowers matrix values a vertex shader computes, as PPSSPP's hardware skinning makes them
(ppsspp:GPU/Common/VertexShaderGenerator.cpp): OpMatrixTimesScalar, each bone by its weight, and OpCompositeExtract of a
matrix value's column, as glslang adds matrices a column at a time. xost failed on every skinning shader PPSSPP makes,
1 to 8 bones: "is not a matrix value" for one bone and "unhandled op CompositeExtract" for more.

- vk_skinning.vert: four bones by four weights, summed, the position and a normal (w 0) through the sum.
- vk_skinning_one.vert: one bone by its weight, kept in a local.
- vk_skinning_rows.vert: two bones held row_major, whose columns are gathered a component a row.
- On the simulator, optimiser off and on, each at five sets of weights: the position and normal within a part in 10^5
  of the sums worked out here. XenosRecomp's oracle accepts each.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
POSITION = [0.75, -1.5, 2.25]
WEIGHTS = ([1.0, 0.0, 0.0, 0.0], [0.25, 0.25, 0.25, 0.25], [0.5, 0.3, 0.15, 0.05], [0.0, 0.0, 0.0, 1.0],
           [0.7, 0.2, 0.1, 0.0])
# Each shader: its bones, whether they're held as rows, and whether it writes the normal.
SHADERS = {
    "vk_skinning.vert": (4, False, True),
    "vk_skinning_one.vert": (1, False, False),
    "vk_skinning_rows.vert": (2, True, False),
}


def bone(k):
    """Bone k's three columns of four, each entry its own."""
    return [[0.5 + 0.25 * k - 0.125 * j + 0.0625 * r * (j + 1) - 0.03125 * k * r for r in range(4)] for j in range(3)]


def through(v, columns):
    """v * M for a mat3x4 of columns: each component the dot of v with a column."""
    return [sum(a * b for a, b in zip(v, column)) for column in columns]


def want(bones, weights, normal):
    skin = [[sum(weights[k] * bone(k)[j][r] for k in range(bones)) for r in range(4)] for j in range(3)]
    scale = 1.9921875 if normal else 1.0
    position = [x * scale for x in through(POSITION + [1.0], skin)] + [1.0]
    return position, through([POSITION[1], POSITION[2], POSITION[0], 0.0], skin)


def constants_for(block, bones, rows):
    """The block as std140 holds it: each bone 3 columns, or 4 rows of 3 when row_major, then the position."""
    constants = {}
    per = 4 if rows else 3
    for k in range(bones):
        columns = bone(k)
        for i in range(per):
            constants[block + per * k + i] = [columns[j][i] for j in range(3)] + [0.0] if rows else columns[i]
    constants[block + per * bones] = POSITION + [0.0]
    return constants


def close(got, wanted):
    return got is not None and len(got) >= len(wanted) and all(
        math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-6) for a, b in zip(got, wanted))


def check(name):
    bones, rows, normal = SHADERS[name]
    for optimize in (False, True):
        blob, manifest, resources = native.compile_spirv((TD / f"{name}.spv").read_bytes(), XSDK_REGISTERS.pixel,
                                                         XSDK_REGISTERS.vertex, optimize, True)
        mode = f"{name}, optimiser {'on' if optimize else 'off'}"
        block = next(r for r in resources if r[0] == "uniform")
        constants = {reg: list(values) for reg, values in (n for n in manifest if isinstance(n[0], int))}
        constants.update(constants_for(block[3], bones, rows))
        for weights in WEIGHTS:
            result = simulate.run(blob, {0: [0.0] * 4}, constants, vertex_fetch=lambda n, fetch, i: weights)
            position, wanted_normal = want(bones, weights, normal)
            if not close(result.exports.get(OPOS), position):
                print(f"  FAIL {mode}: weights {weights}: position {result.exports.get(OPOS)}, want {position}")
                return False
            if normal and not close(result.exports.get(0), wanted_normal):
                print(f"  FAIL {mode}: weights {weights}: normal {result.exports.get(0)}, want {wanted_normal}")
                return False
        if oracle_hlsl(blob, f"skinning_{name.replace('.', '_')}_{int(optimize)}") is None:
            print(f"  FAIL {mode}: XenosRecomp rejected it")
            return False
    print(f"  PASS {name}: {len(WEIGHTS)} sets of weights through {bones} bone{'s' if bones > 1 else ''}, optimiser "
          f"off and on; the oracle accepts it")
    return True


def main():
    results = []
    for name in SHADERS:
        try:
            results.append(check(name))
        except native.CompileError as error:
            print(f"  FAIL {name}: {native.STATUS_NAMES[error.status]}: {error}")
            results.append(False)
    if not all(results):
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
