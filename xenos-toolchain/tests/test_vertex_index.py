#!/usr/bin/env python3
"""xsc lowers gl_VertexIndex, Vulkan's VertexIndex, firstVertex or vertexOffset included, and exact int shifts and
ands on it.

- The vertex base: one float constant register past the blocks and the instance group, before the literals, the
  resource ("vertex_base", 0, 0, register, 1). The runtime writes its x, what Vulkan's index adds to the GPU's: the
  draw's first vertex or vertex offset, as it moves the streams by it.
- gl_VertexIndex is r0.x, the GPU's vertex index as a float (xenia:src/xenia/gpu/spirv_shader_translator.cc:
  1500-1517), or an instanced shader's t.x, plus the base's x; the fetches leave r0 as the GPU filled it.
- On the simulator, optimiser off and on, at bases 0, 1, 5, 4,096, -3 and -1,000 and at indices 0 to 69, 65,535 and up
  to the 24-bit index's last, each export exactly the float nearest what's wanted: vk_vertex_index.vert, its input fetched at r0.x beside gl_VertexIndex;
  vk_vertex_index_quad.vert, PPSSPP's quad from gl_VertexIndex alone, reading its interpolator back;
  vk_vertex_index_bits.vert, shifts by constants and ands with runs of bits from bit 0, from past it, to bit 31, every
  bit and none, each against Python's ints, negative ones among them; vk_vertex_index_instance.vert, instanced, at
  t.x from the instance group of N instances of V.
- XenosRecomp's oracle accepts each.
- vk_vertex_index_mask.vert ands with bits that aren't one run: refused.
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
from test_instancing import group  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
BASES = (0, 1, 5, 4096, -3, -1000)
# The GPU's vertex index holds 24 bits (xenia:src/xenia/gpu/xenos.h:38-42).
LAST_INDEX = (1 << 24) - 1
INDICES = tuple(range(70)) + (65535, 65536, LAST_INDEX - 4096, LAST_INDEX - 5)
# What vk_vertex_index.vert's and vk_vertex_index_instance.vert's input fetch gives at element e.
FETCH_SCALE = 8.0
# N instances of V from a first instance, auto-indexed.
INSTANCED_DRAWS = ((6, 4, 2), (3, 5, 1))


def fetched(index):
    return [FETCH_SCALE * math.floor(index), 1.0, 2.0, 1.0]


def bits(v):
    """vk_vertex_index_bits.vert's two interpolators for gl_VertexIndex v, as Python's ints, two's complement's bits
    for negative ones, work them out."""
    return [v >> 2, v & 0xF0, v & ~3, (v & 7) << 3], [v & -1, v & 0, 3 & v, v >> 0]


def quad(v):
    x, y = float((v & 1) << 1), float(v & 2)
    return [2.0 * x - 1.0, 2.0 * y - 1.0, 0.0, 1.0], [x, y]


# Each shader: what a vertex exports, {export: values}, for gl_VertexIndex v and the GPU's index i.
SHADERS = {
    "vk_vertex_index.vert": lambda v, i: {OPOS: [FETCH_SCALE * i + v, 1.0, 2.0, 1.0]},
    "vk_vertex_index_quad.vert": lambda v, i: dict(zip((OPOS, 0), quad(v))),
    "vk_vertex_index_bits.vert": lambda v, i: {OPOS: [float(v), 0.0, 0.0, 1.0], 0: bits(v)[0], 1: bits(v)[1]},
}


def compiled(name, optimize):
    return native.compile_spirv((TD / f"{name}.spv").read_bytes(), XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex,
                                optimize, True)


def exact(got, want):
    """Each component the float closest to what's wanted, as one rounding of exact values gives."""
    return got is not None and len(got) >= len(want) and all(a == simulate.single(b) for a, b in zip(got, want))


def resources_problem(resources, manifest, instanced):
    """What's wrong with the vertex base among the resources, if anything; its register otherwise."""
    bases = [r for r in resources if r[0] == "vertex_base"]
    groups = [r for r in resources if r[0] == "instance"]
    literals = [reg for reg, _ in (n for n in manifest if isinstance(n[0], int))]
    block_end = max((reg + regs for kind, _, _, reg, regs in resources if kind in ("push", "uniform")), default=0)
    if (len(bases) != 1 or bases[0] != ("vertex_base", 0, 0, bases[0][3], 1) or bases[0][3] < block_end
            or len(groups) != instanced or any(bases[0][3] <= g[3] for g in groups)
            or any(r <= bases[0][3] for r in literals) or resources[-1] != bases[0]):
        return (f"resources {resources}, literals at {sorted(literals)}: want the base last, alone, past the blocks "
                f"and the group and before the literals")
    return bases[0][3]


def check_shader(name):
    expect = SHADERS[name]
    for optimize in (False, True):
        blob, manifest, resources = compiled(name, optimize)
        mode = f"{name}, optimiser {'on' if optimize else 'off'}"
        reg = resources_problem(resources, manifest, False)
        if isinstance(reg, str):
            print(f"  FAIL {mode}: {reg}")
            return False
        constants = {r: list(values) for r, values in (n for n in manifest if isinstance(n[0], int))}
        for base in BASES:
            constants[reg] = [float(base), 0.0, 0.0, 0.0]
            for index in (i for i in INDICES if abs(i + base) <= LAST_INDEX):
                result = simulate.run(blob, {0: [float(index), 0.0, 0.0, 0.0]}, constants,
                                      vertex_fetch=lambda n, fetch, i: fetched(i))
                for export, want in expect(index + base, index).items():
                    got = result.exports.get(export)
                    if not exact(got, want):
                        print(f"  FAIL {mode}: index {index} at base {base}, export {export}: {got}, want {want}")
                        return False
        if oracle_hlsl(blob, f"vertex_index_{name.replace('.', '_')}_{int(optimize)}") is None:
            print(f"  FAIL {mode}: XenosRecomp rejected it")
            return False
    print(f"  PASS {name}: the base at c{reg}; {len(INDICES)} indices at {len(BASES)} bases right, optimiser off and "
          f"on; the oracle accepts it")
    return True


def check_instanced():
    name = "vk_vertex_index_instance.vert"
    for optimize in (False, True):
        blob, manifest, resources = compiled(name, optimize)
        mode = f"{name}, optimiser {'on' if optimize else 'off'}"
        reg = resources_problem(resources, manifest, True)
        if isinstance(reg, str):
            print(f"  FAIL {mode}: {reg}")
            return False
        group_reg = next(r[3] for r in resources if r[0] == "instance")
        constants = {r: list(values) for r, values in (n for n in manifest if isinstance(n[0], int))}
        for base in BASES:
            constants[reg] = [float(base), 0.0, 0.0, 0.0]
            for vertices, instances, first in INSTANCED_DRAWS:
                constants[group_reg] = group(vertices, first)
                for k in range(instances):
                    for j in range(vertices):
                        result = simulate.run(blob, {0: [float(k * vertices + j), 0.0, 0.0, 0.0]}, constants,
                                              vertex_fetch=lambda n, fetch, i: fetched(i))
                        want = [FETCH_SCALE * j + j + base, 1.0 + k + first, 2.0, 1.0]
                        got = result.exports.get(OPOS)
                        if not exact(got, want):
                            print(f"  FAIL {mode}: vertex {j} of instance {k} of {instances} of {vertices} from "
                                  f"{first} at base {base}: {got}, want {want}")
                            return False
        if oracle_hlsl(blob, f"vertex_index_instance_{int(optimize)}") is None:
            print(f"  FAIL {mode}: XenosRecomp rejected it")
            return False
    print(f"  PASS {name}: the base at c{reg} past the group at c{group_reg}; every vertex of "
          f"{', '.join(f'{n} of {v} from {f}' for v, n, f in INSTANCED_DRAWS)} at {len(BASES)} bases right, optimiser "
          f"off and on; the oracle accepts it")
    return True


def check_refused():
    name = "vk_vertex_index_mask.vert"
    try:
        compiled(name, False)
    except native.CompileError as error:
        if error.status != native.REFUSED:
            print(f"  FAIL {name}: failed as {native.STATUS_NAMES[error.status]}, not refused: {error}")
            return False
        print(f"  PASS {name}: refused ({error})")
        return True
    print(f"  FAIL {name}: lowered an and with bits that aren't one run")
    return False


def main():
    results = [check_shader(name) for name in SHADERS]
    results += [check_instanced(), check_refused()]
    if not all(results):
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
