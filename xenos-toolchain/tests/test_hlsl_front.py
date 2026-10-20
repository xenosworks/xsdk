#!/usr/bin/env python3
"""What glslang's HLSL brings xost's front (xsdk:docs/scope/50-hlsl.md, step 2): static globals, which are Private
variables, as locals (xsdk:src/xsc_spirv.c, xsdk_xsc_declares_local); clamp's bounds folded to constant splats
(src/xsc_lower.c, xsdk_xsc_const_scalar); OpUndef and OpConstantNull as zeros (src/xsc_spirv.c,
xsdk_xsc_spirv_zero_nulls); and a float2 FragCoord used whole, which has no z or w (src/xsc_lower_fragment.c,
reads_depth). And step 4's guard: a Private variable given an initializer, which xost would drop, is refused
(src/xsc_compile.c, refuse_initializers); glslang writes a static's value as a store.

Each shader of xsc/testdata/hlsl, whose SPIR-V must hold what it's there for, runs on xdis/simulate.py, the optimiser
off and on, and each export must be what the HLSL gives exactly; no operation rounds on these inputs. A static global
stored twice takes the unroller's locals, the rest the lowering's. An undefined component is 0, as xost makes it.
gl_FragCoord is the pixel parameters' |xy| plus c255, which xsdk keeps at 0.5.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from test_local_stores import module  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402

UV = [0.375, -1.25, 0.0, 0.0]
COLOUR = [0.125, 0.5, -0.75, 1.5]
K = [0.5, 2.0, -4.0, 0.25]
V = [-1.0, 0.25, 0.9, 0.75]
W = [0.1, 3.0, 0.0, 0.0]
PIXEL = [37.0, 12.0, 0.0, 0.0]
PRIVATE, FCLAMP, CONSTANT_COMPOSITE, UNDEF, CONSTANT_NULL, FRAG_COORD = 6, 43, 44, 1, 46, 15


def clamp(x, lo, hi):
    return min(max(x, lo), hi)


def holds(m, what):
    """Whether the SPIR-V has the construct the shader is there for."""
    words = struct.unpack(f"<{len(m.blob) // 4}I", m.blob)[5:]
    opcodes = []
    while words:
        opcodes.append((words[0] & 0xFFFF, words[1:words[0] >> 16]))
        words = words[words[0] >> 16:]
    if what == "private":
        return any(op == 59 and ops[2] == PRIVATE for op, ops in opcodes)
    if what == "splat":
        splats = {ops[1] for op, ops in opcodes if op == CONSTANT_COMPOSITE and len(set(ops[2:])) == 1}
        return any(op == 12 and ops[3] == FCLAMP and splats & set(ops[5:]) for op, ops in opcodes)
    if what == "undefined":
        return {UNDEF, CONSTANT_NULL} <= {op for op, _ in opcodes}
    frag_coord = next(ops[0] for op, ops in opcodes if op == 71 and ops[1:3] == (11, FRAG_COORD))
    pointer = next(ops for op, ops in opcodes if op == 59 and ops[1] == frag_coord)[0]
    pointee = next(ops for op, ops in opcodes if op == 32 and ops[0] == pointer)[2]
    return any(op == 23 and ops[0] == pointee and ops[2] == 2 for op, ops in opcodes)


# name -> (what it holds, interpolators and pixel parameters, constants, exports wanted). TEXCOORD1 is interpolator 2,
# past COLOR0's (xsdk:src/xsc_resolve.c, interpolator_of).
CASES = {
    "static_globals_ps.hlsl": ("private", {0: UV, 2: COLOUR}, {0: K},
                               {0: [2.0 * K[i] * UV[0] + 2.0 * UV[i % 2] - 0.5 * COLOUR[i] for i in range(4)]}),
    "splat_clamp_ps.hlsl": ("splat", {0: V, 2: W}, {},
                            {0: [clamp(V[i], -0.5, 0.75) + clamp(W[i % 2], 0.25, 2.0) for i in range(4)]}),
    "undefined_ps.hlsl": ("undefined", {0: UV}, {0: K},
                          {0: [UV[0] * K[0], UV[1] * K[1], 0.0, 0.0], 1: [0.0] * 4}),
    "float2_position_ps.hlsl": ("float2", {0: UV, 1: PIXEL}, {0: K},
                                {0: [(PIXEL[i] + 0.5) * K[i] + UV[i] for i in range(2)] +
                                 [PIXEL[0] + 0.5, PIXEL[1] + 0.5]}),
}


OP_VARIABLE, OP_CONSTANT = 59, 43


def with_initializer(blob):
    """The module with its first Private variable given its first constant as an initializer."""
    words = list(struct.unpack(f"<{len(blob) // 4}I", blob))
    at, constant = 5, None
    while at < len(words):
        count, opcode = words[at] >> 16, words[at] & 0xFFFF
        if opcode == OP_CONSTANT and constant is None:
            constant = words[at + 2]
        if opcode == OP_VARIABLE and count == 4 and words[at + 3] == PRIVATE and constant is not None:
            words[at] = (count + 1) << 16 | opcode
            words.insert(at + count, constant)
            return struct.pack(f"<{len(words)}I", *words)
        at += count
    raise ValueError("no Private variable after a constant")


def initializer_refused():
    m = module("hlsl/static_globals_ps.hlsl")
    lower_ex(m, XSDK_REGISTERS, True)
    try:
        native.compile_spirv(with_initializer(m.blob), XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, True)
    except native.CompileError as e:
        if e.status != native.REFUSED or "with an initializer" not in str(e):
            raise SystemExit(f"  FAIL an initializer: {e.status}, {e}")
    else:
        raise SystemExit("  FAIL a Private variable with an initializer compiled")
    print("  PASS a Private variable with an initializer refused")


def main():
    initializer_refused()
    for name, (what, registers, constants, wants) in CASES.items():
        m = module(f"hlsl/{name}")
        if not holds(m, what):
            raise SystemExit(f"  FAIL {name}: its SPIR-V holds no {what}, so the test can't tell")
        for optimize in (False, True):
            tag = f"optimiser {'on' if optimize else 'off'}"
            blob, manifest = lower_ex(m, XSDK_REGISTERS, optimize)
            literals = {entry[0]: list(entry[1]) for entry in manifest if len(entry) == 2}
            literals[native.FRAG_COORD_CONSTANT] = [0.5] * 4
            got = simulate.run(blob, registers, {**constants, **literals}).exports
            want = {reg: [simulate.single(v) for v in values] for reg, values in wants.items()}
            if any(got.get(reg) != values for reg, values in want.items()):
                raise SystemExit(f"  FAIL {name}, {tag}: exports {got} where the HLSL gives {want}")
            print(f"  PASS {name}, {tag}: exports exact")
    print("OK")


if __name__ == "__main__":
    main()
