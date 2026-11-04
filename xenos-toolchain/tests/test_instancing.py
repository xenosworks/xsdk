#!/usr/bin/env python3
"""xsc lowers a vertex shader for instanced draws (xsdk:docs/scope/47-instancing.md, step 2): one that reads
gl_InstanceIndex always, any other with the instanced option.

- The group: one float constant register past the blocks' groups of 4 and before the literals, the resource
  ("instance", 0, 0, register, 1), and _instance in the container's constant table. The runtime writes it before each
  piece of a draw: x 1/S, y 0.5/S (0 for S a power of two), z S, w the piece's first instance, S the vertices an
  instance auto-indexed or the copies' step of a repeated index buffer.
- The prologue, k = floor(r0.x * c.x + c.y), t.x = r0.x - c.z * k, t.y = k + c.w, is an exec of ALU alone; the next
  exec leads with the vertex fetches, a vertex-cache bit each and the first serialized, as the console drew it
  (xsdk:docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1"). Every vertex fetch reads t.x; the runtime
  points one at the instance's rate at t.y by writing 1 to its source swizzle, bits 30 and 31 of its first word
  (xenia:src/xenia/gpu/ucode.h:758, shader_interpreter.cc:1009-1012).
- On the simulator, optimiser off and on, every vertex of N instances of V auto-indexed, and of copies of a quad's 6
  indices moved on by 4: each fetch patched to a stream of its own as the runtime will, the fetch callback giving the
  stream's element at the index the fetch floors, each vertex's exports checked. vk_instance_index.vert reads
  gl_InstanceIndex past a push and a uniform block, its colour a uniform array's element at it; vk_instance_alone.vert
  has no block; vk_instance_rate.vert has attributes at the instance's rate alone; vk_instance_both.vert both. Then,
  for vk_instance_rate.vert, the counts that leave the division least room, as the probe drew them.
- The division as float32 works it out: for every V from 1 to 65,535, at each instance's first and last vertex in a
  piece of whole instances up to 65,535 vertices, the two closest to a wrong floor, floor(i * 1/V + 0.5/V) is the
  instance, the multiply-add fused, or rounded after the multiply and the add to nearest or toward zero; and for S each
  power of two to 2^23, y 0, at the first, middle and last copies' first and last index, up to 2^24 - 1.
- XenosRecomp's oracle accepts each instanced shader.
- Without the option, vk_instance_rate.vert isn't instanced: no group, every fetch at r0.x.
gl_VertexIndex, in an instanced shader too, is tests/test_vertex_index.py's.
"""
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.container_emit as ce  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from test_optimize import vertex_elements  # noqa: E402
from test_xsc_lower import TD, oracle_hlsl  # noqa: E402

OPOS = int(xu.ExportRegister.kVSPosition)
# The console loads constants in groups of 4 (xsdk:docs/HARDWARE.md, "Constants loaded from memory go in groups of 4
# too").
CONSTANT_GROUP = 4
# A draw packet counts 16 bits of vertices or indices (xenia:src/xenia/gpu/registers.h:327); a vertex index holds 24
# bits (xenos.h:38-42).
MAX_DRAW = 65535
INDEX_BITS = 24
# Stream n's vertex fetch constant, as xsdk binds it (xsdk:src/shader.c, STREAM_0_FETCH_CONSTANT), and a fetch's
# fields: the constant, its source swizzle (xenia:src/xenia/gpu/ucode.h:745-758).
STREAM_0 = 95
CONSTANT_SHIFT, SELECT_SHIFT, SWIZZLE_SHIFT = 20, 25, 30
CONSTANT_FIELDS = 0x7F << CONSTANT_SHIFT | 3 << SWIZZLE_SHIFT
T_Y = 1
# An exec's sequence, two bits an instruction, fetch then serialize (ucode.h:239-259), and its opcodes.
EXEC_OPCODES = (1, 2)
FETCH_BITS = 0x555
QUAD_INDICES = (0, 1, 2, 2, 1, 3)
QUAD_STEP = 4
# N instances of V from a first instance, auto-indexed; copies of the quad's indices.
AUTO_DRAWS = ((6, 4, 2), (3, 5, 1))
COPIES, COPIES_FIRST = 5, 1
# The probe's counts that leave the division least room, and its indexed forms, (V, S, copies) (tools/
# gen_instancing_probe.py), from instance 5.
LEAST_ROOM = (3, 7, 255, 257, 4369, 21845, 65535)
INDEXED_FORMS = ((2, 2, 32767), (3, 4, 21845), (129, 256, 508), (2049, 4096, 31), (5, 1 << 16, 255),
                 (16, 1 << 20, 16), (1, 1 << 23, 2))
LEAST_ROOM_FIRST = 5
POSITION_0, COLOR_0 = (ce.USAGE_POSITION, 0), (ce.USAGE_COLOR, 0)
TEXCOORD_0, TEXCOORD_1 = (ce.USAGE_TEXCOORD, 0), (ce.USAGE_TEXCOORD, 1)
SCALE = [2.0, 4.0, 1.0, 1.0]
COLORS = [[0.125 * k, 1.0 - 0.125 * k, 0.5, 1.0] for k in range(8)]


def element(stream, e):
    """Stream n's element e: e in lane n, n + 1 in w."""
    return [float(e) if lane == stream else 0.0 for lane in range(3)] + [stream + 1.0]


def add(*vectors):
    return [sum(lanes) for lanes in zip(*vectors)]


# Each shader: its compile's instanced option, each input's (usage, usage index) -> (stream, at the instance's rate),
# the constants besides the group and the literals, and a vertex's (position, colour) for vertex j of instance k, the
# first instance counted in. vk_instance_index.vert's colours have 8 entries, so its instances stay below 8.
SHADERS = {
    "vk_instance_index.vert": (False, {POSITION_0: (0, False)}, {0: SCALE, **{4 + k: c for k, c in enumerate(COLORS)}},
                               lambda j, k: ([2.0 * j + 0.5 * (k % 4), 0.5 * (k // 4), float(k), 1.0], COLORS[k])),
    "vk_instance_alone.vert": (False, {POSITION_0: (0, False)}, {},
                               lambda j, k: ([j + 0.25 * k, 0.0, 0.0, 1.0], None)),
    "vk_instance_rate.vert": (True, {POSITION_0: (0, False), TEXCOORD_0: (1, True), TEXCOORD_1: (2, True)}, {},
                              lambda j, k: (add(element(0, j), element(1, k)), element(2, k))),
    "vk_instance_both.vert": (False, {POSITION_0: (0, False), COLOR_0: (1, False), TEXCOORD_0: (2, True)}, {},
                              lambda j, k: (add(element(0, j), element(2, k), [0.0, 0.0, float(k), 0.0]),
                                            [x * (k + 1) for x in element(1, j)])),
}


def compiled(name, optimize, instanced=False):
    return native.compile_spirv((TD / f"{name}.spv").read_bytes(), XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex,
                                optimize, True, instanced)


def group(divisor, first):
    """The group's x to w for S and the piece's first instance, 1/S and 0.5/S rounded to floats as the CPU works them
    out, 0.5/S 0 for a power of two."""
    power = divisor & (divisor - 1) == 0
    return [simulate.single(1 / divisor), 0.0 if power else simulate.single(0.5 / divisor), float(divisor),
            float(first)]


def patched(blob, inputs):
    """The blob with each fetch at the stream its input names, and those at the instance's rate at t.y, as the
    runtime's patch will point them (xsdk:src/shader.c, xsdk_shader_set_vertex_layout)."""
    c = container.Container(blob)
    out = bytearray(blob)
    for slot, usage, usage_index in vertex_elements(blob):
        stream, per_instance = inputs[(usage, usage_index)]
        at = c.offset + c.sh_off + slot * 12
        constant = STREAM_0 - stream
        word = struct.unpack_from(">I", out, at)[0] & ~CONSTANT_FIELDS
        word |= constant // 3 << CONSTANT_SHIFT | constant % 3 << SELECT_SHIFT
        struct.pack_into(">I", out, at, word | (T_Y if per_instance else 0) << SWIZZLE_SHIFT)
    return bytes(out)


def fetched(fetch, index):
    return element(STREAM_0 - fetch.fetch_constant_index(), math.floor(index))


def close(got, want):
    return got is not None and len(got) == len(want) and all(abs(a - b) <= 1e-5 * max(1.0, abs(b))
                                                            for a, b in zip(got, want))


def draw_problem(blob, reg, constants, expect, points, first):
    """What's wrong with the exports of each point, (index r0.x holds, S, vertex, instance), if anything."""
    for index, divisor, vertex, instance in points:
        constants[reg] = group(divisor, first)
        result = simulate.run(blob, {0: [float(index), 0.0, 0.0, 0.0]}, constants,
                              vertex_fetch=lambda n, fetch, i: fetched(fetch, i))
        position, colour = expect(vertex, instance + first)
        got = result.exports.get(OPOS), result.exports.get(0)
        if not close(got[0], position) or (colour is not None and not close(got[1], colour)):
            return (f"index {index} at S {divisor}, vertex {vertex} of instance {instance}: position {got[0]} "
                    f"(want {position}), colour {got[1]} (want {colour})")
    return None


def auto_points(vertices, instances):
    return [(k * vertices + j, vertices, j, k) for k in range(instances) for j in range(vertices)]


def ends(count):
    """A count's first two, middle and last two: the first and last, the places closest to a wrong floor."""
    return sorted({n for n in (0, 1, count // 2, count - 2, count - 1) if 0 <= n < count})


def execs(words):
    """Each exec's address, count, sequence and vertex-cache mask, the control flow ending where the first exec's
    instructions start."""
    out, end, slot = [], len(words) // 3, 0
    while slot < end:
        a, b, c = words[3 * slot:3 * slot + 3]
        for low, high in ((a, b & 0xFFFF), (b >> 16 | (c & 0xFFFF) << 16, c >> 16)):
            if high >> 12 & 0xF in EXEC_OPCODES:
                out.append((low & 0xFFF, low >> 12 & 7, low >> 16 & 0xFFF, (high & 3) << 4 | low >> 28))
                end = min(end, low & 0xFFF)
        slot += 1
    return out


def layout_problem(blob):
    """What's wrong with the prologue's exec and the fetches', if anything: an exec of ALU alone writing t.x and t.y,
    then one leading with its vertex fetches, a vertex-cache bit each, the first serialized, every fetch at t.x."""
    words = container.Container(blob).ucode_dwords
    records = {rec.offset: rec for rec in xu._decode_program(words, "vertex")[0] if rec.kind != "cf"}
    (address, count, sequence, _), (fetch_address, fetch_count, fetch_sequence, vc) = execs(words)[:2]
    if sequence & FETCH_BITS:
        return f"the prologue's exec, {count} instructions, holds a fetch"
    leading = next((n for n in range(fetch_count) if not fetch_sequence >> 2 * n & 1), fetch_count)
    vfetches = [rec for rec in records.values() if rec.kind == "vfetch"]
    if not leading or leading != len(vfetches) or vc != (1 << leading) - 1 or not fetch_sequence & 2:
        return (f"the fetches' exec leads with {leading} of the {len(vfetches)} fetches, vertex-cache mask {vc:#x}, "
                f"sequence {fetch_sequence:#x}")
    sources = {(f.src(), f.src_swizzle()) for f in (xu.VertexFetchInstruction(*rec.raw) for rec in vfetches)}
    written = set()
    for slot in range(address, address + count):
        alu = xu.AluInstruction(*records[slot].raw)
        written |= {(alu.vector_dest(), lane) for lane in range(4) if alu.vector_write_mask() >> lane & 1}
        written |= {(alu.scalar_dest(), lane) for lane in range(4) if alu.scalar_write_mask() >> lane & 1}
    t = min(sources)[0]
    if sources != {(t, 0)} or not {(t, 0), (t, 1)} <= written or fetch_address != address + count:
        return f"the fetches read {sorted(sources)}, the prologue writes {sorted(written)}"
    return None


def check_shader(name):
    instanced, inputs, extra, expect = SHADERS[name]
    for optimize in (False, True):
        blob, manifest, resources = compiled(name, optimize, instanced)
        mode = f"{name}, optimiser {'on' if optimize else 'off'}"
        groups = [r for r in resources if r[0] == "instance"]
        literals = {reg: list(values) for reg, values in (n for n in manifest if isinstance(n[0], int))}
        block_end = max((reg + regs for kind, _, _, reg, regs in resources if kind in ("push", "uniform")), default=0)
        if (len(groups) != 1 or groups[0] != ("instance", 0, 0, groups[0][3], 1) or groups[0][3] % CONSTANT_GROUP
                or groups[0][3] < block_end or any(r <= groups[0][3] for r in literals) or b"_instance\0" not in blob):
            print(f"  FAIL {mode}: resources {resources}, literals at {sorted(literals)}: want the group alone, past "
                  f"the blocks' groups of 4 and before the literals, named _instance")
            return False
        reg = groups[0][3]
        problem = layout_problem(blob)
        draws = [(f"{n} of {v} from {f}", auto_points(v, n), f) for v, n, f in AUTO_DRAWS]
        draws.append((f"{COPIES} copies of the quad from {COPIES_FIRST}",
                      [(c * QUAD_STEP + m, QUAD_STEP, m, c) for c in range(COPIES) for m in QUAD_INDICES],
                      COPIES_FIRST))
        for what, points, first in draws if not problem else ():
            problem = draw_problem(patched(blob, inputs), reg, {**literals, **extra}, expect, points, first)
            problem = problem and f"{what}: {problem}"
            if problem:
                break
        if not problem and oracle_hlsl(blob, f"instancing_{name.replace('.', '_')}_{int(optimize)}") is None:
            problem = "XenosRecomp rejected it"
        if problem:
            print(f"  FAIL {mode}: {problem}")
            return False
    print(f"  PASS {name}: the group at c{reg}, the prologue an exec of ALU alone and the fetches at t.x after it; "
          f"every vertex of {', '.join(d[0] for d in draws)} right, optimiser off and on; the oracle accepts it")
    return True


def check_least_room():
    """vk_instance_rate.vert at the probe's counts, each count's first and last instances and vertices."""
    name = "vk_instance_rate.vert"
    _, inputs, _, expect = SHADERS[name]
    points = [(k * v + j, v, j, k) for v in LEAST_ROOM for k in ends(MAX_DRAW // v) for j in ends(v)]
    points += [(c * s + j, s, j, c) for v, s, n in INDEXED_FORMS for c in ends(n) for j in ends(v)]
    for optimize in (False, True):
        blob, manifest, resources = compiled(name, optimize, True)
        reg = next(r[3] for r in resources if r[0] == "instance")
        literals = {r: list(values) for r, values in (n for n in manifest if isinstance(n[0], int))}
        problem = draw_problem(patched(blob, inputs), reg, literals, expect, points, LEAST_ROOM_FIRST)
        if problem:
            print(f"  FAIL {name}, optimiser {'on' if optimize else 'off'}, least room: {problem}")
            return False
    print(f"  PASS {name}: the probe's {len(LEAST_ROOM)} counts and {len(INDEXED_FORMS)} indexed forms from instance "
          f"{LEAST_ROOM_FIRST}, {len(points)} points, right optimiser off and on")
    return True


def singles(values):
    """values each rounded to a float, to nearest."""
    return struct.unpack(f"<{len(values)}f", struct.pack(f"<{len(values)}f", *values))


def toward_zero(values):
    """values each rounded to a float toward zero; they're all at least 0."""
    near = singles(values)
    bits = list(struct.unpack(f"<{len(near)}I", struct.pack(f"<{len(near)}f", *near)))
    for n, (value, rounded) in enumerate(zip(values, near)):
        bits[n] -= rounded > value
    return struct.unpack(f"<{len(bits)}f", struct.pack(f"<{len(bits)}I", *bits))


def check_division():
    """floor(i * x + y) at each instance's first and last vertex, x and y the group's, for every V; and for S each
    power of two to 2^23."""
    index, x, y, want = [], [], [], []
    for v in range(1, MAX_DRAW + 1):
        gx, gy, _, _ = group(v, 0)
        for k in range(MAX_DRAW // v):
            index += [k * v, k * v + v - 1]
            want += [k, k]
        x += [gx] * (len(index) - len(x))
        y += [gy] * (len(index) - len(y))
    for p in range(INDEX_BITS):
        s = 1 << p
        for c in ends((1 << INDEX_BITS) // s):
            index += [c * s, c * s + s - 1]
            want += [c, c]
            x += [2.0 ** -p] * 2
            y += [0.0, 0.0]
    products = [i * gx for i, gx in zip(index, x)]
    models = {
        "fused": singles([i * gx + gy for i, gx, gy in zip(index, x, y)]),
        "rounded to nearest": singles([q + gy for q, gy in zip(singles(products), y)]),
        "rounded toward zero": toward_zero([q + gy for q, gy in zip(toward_zero(products), y)]),
    }
    room = 1.0
    for model, quotients in models.items():
        wrong = [n for n, (q, k) in enumerate(zip(quotients, want)) if math.floor(q) != k]
        if wrong:
            n = wrong[0]
            print(f"  FAIL the division, {model}: index {index[n]}, x {x[n]}, y {y[n]}: floor {quotients[n]} isn't "
                  f"{want[n]} ({len(wrong)} wrong)")
            return False
        # The room left to a wrong floor, in units of 1/S: 0.5 auto-indexed in exact arithmetic.
        room = min(room, min(min(q - k, k + 1 - q) / gx for q, k, gx, gy in zip(quotients, want, x, y) if gy))
    print(f"  PASS the division: {len(index)} indices, every V to {MAX_DRAW} and S to 2^{INDEX_BITS - 1}, fused and "
          f"rounded to nearest and toward zero; auto-indexed, the least room to a wrong floor {room:.4f}/V")
    return True


def check_plain():
    name = "vk_instance_rate.vert"
    blob, _, resources = compiled(name, False)
    vfetches = [xu.VertexFetchInstruction(*rec.raw) for rec in
                xu._decode_program(container.Container(blob).ucode_dwords, "vertex")[0] if rec.kind == "vfetch"]
    if resources or b"_instance\0" in blob or any((f.src(), f.src_swizzle()) != (0, 0) for f in vfetches):
        print(f"  FAIL {name} without the option: resources {resources}, fetches at "
              f"{[(f.src(), f.src_swizzle()) for f in vfetches]}")
        return False
    print(f"  PASS {name} without the option: no group, its {len(vfetches)} fetches at r0.x")
    return True


def main():
    results = [check_shader(name) for name in SHADERS]
    results += [check_least_room(), check_division(), check_plain()]
    if not all(results):
        raise SystemExit("FAILED")
    print("OK")


if __name__ == "__main__":
    main()
