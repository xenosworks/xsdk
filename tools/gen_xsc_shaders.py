#!/usr/bin/env python3
"""Generate xsdk_test's check of xost's compiler on the console (tests/test_xsc.c): shaders' SPIR-V from xost's
testdata, with the container, literals, clip planes and resources the compiler makes from each on the PC, at xsdk's
register split with the optimiser on, as Dusklight's pack is built. vk_block_literals.vert reads a push and a uniform
block and pools a literal; vk_texel_offsets.frag samples four times, laid out with its coordinates' work first and the
fetches together; vk_vertex_if.vert has if/else arms on data the unroller merges into selects, vk_vertex_uniform_if.vert
arms on a push constant merged the same way, and vk_loop_count_if.frag a hardware loop a uniform buffer's int counts
(vulkan4xenon milestone 21); vulkan4xenon milestone 26's vk_clip_distance.vert writes three clip distances from planes
in its blocks, and vk_point_sprite.vert and vk_point_coord.frag a point's size and its sprite's coordinates, which the
raster test draws with too, as it does vk_dynamic_interpolator.vert, which reads a colour at a0 past the interpolators'
alloc, a0 loaded again before it, vk_dynamic_texcoord.frag, which samples at a coordinate read at a0 that fetch
grouping keeps past its load, and vk_dynamic_if.frag, which reads at a0 in and past predicated if/else arms, in plain
execs as they use a0, one of which loads a0 at another index, and vk_slow_fetch.frag, whose fetches far across a large
texture are read in a later exec; and milestone 44's vk_frag_coord_rows.frag, the pixel's place as its colour, which
the tiles test draws across bands; and milestone 46's vk_loop_if_break.frag, vk_loop_grid.frag and
vk_vertex_loop_break.vert, breaks and continues on data in unrolled loops, the grid's pixels each leaving at their own
pass in the raster test, and vk_loop_count_grid.frag, vk_loop_break_data.frag, vk_loop_count_stop.frag and
vk_vertex_loop_count_break.vert, the same in hardware loops, the grid drawn and timed in the raster test too, and
vk_loop_float_count.frag, vk_loop_data_count.frag, vk_loop_while.frag, vk_loop_count_skip.frag,
vk_vertex_loop_data.vert and vk_loop_data_grid.frag, loops whose count is data and block-counted ones from past 0 or by
past 1, the grid drawn in the raster test, and vk_loop_nest2.frag, vk_loop_nest4.frag, vk_loop_in_data_if.frag,
vk_loop_in_uniform_if.frag, vk_loop_in_unrolled.frag, vk_vertex_loop_nest.vert and vk_loop_nest_grid.frag, hardware
loops nested, in an if/else and in an unrolled loop, the grid drawn in the raster test, and vk_loop_step_grid.frag,
loops from 100 and by 2 to a block's int, each its own passes, drawn in the raster test too, and
vk_loop_sample_lod.frag, vk_loop_sample_grad.frag, vk_loop_sample_computed.frag, vk_loop_derivative.frag,
vk_loop_parallax.frag, vk_vertex_loop_sample.vert and vk_loop_sample_grid.frag, samples and derivatives in hardware
loops' passes, the grid drawn in the raster test; and vk_scalar_input_loop.frag, vk_scalar_input.frag and
vk_vertex_scalar_input.vert, scalar inputs, held in x alone, read as values; and milestone 47's vk_instance_cells.vert
and vk_instance_step.vert, instanced, which the instancing test draws; and milestone 48's vk_indexed_reads.vert, rows
read at two run-time indices that the optimiser reads at a0 where they're used, which the indexed reads test draws;
and milestone 50's HLSL grid, hlsl/grid_*.hlsl, glslang's SPIR-V of D3D9's semantics, which the HLSL test draws.
Prints a C header. Runs in WSL, where make builds the compiler for the PC.
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import xsc.native as native  # noqa: E402

TESTDATA = ROOT / "xenos-toolchain" / "xsc" / "testdata"
SHADERS = ("vk_block_literals.vert", "vk_texel_offsets.frag", "vk_vertex_if.vert", "vk_vertex_uniform_if.vert",
           "vk_loop_count_if.frag", "vk_clip_distance.vert", "vk_point_sprite.vert", "vk_point_coord.frag",
           "vk_dynamic_interpolator.vert", "vk_dynamic_texcoord.frag", "vk_dynamic_if.frag", "vk_slow_fetch.frag",
           "vk_frag_coord_rows.frag", "vk_loop_if_break.frag", "vk_loop_grid.frag", "vk_vertex_loop_break.vert",
           "vk_loop_count_grid.frag", "vk_loop_break_data.frag", "vk_loop_count_stop.frag",
           "vk_vertex_loop_count_break.vert", "vk_loop_float_count.frag", "vk_loop_data_count.frag",
           "vk_loop_while.frag", "vk_loop_count_skip.frag", "vk_vertex_loop_data.vert", "vk_loop_data_grid.frag",
           "vk_loop_nest2.frag", "vk_loop_nest4.frag", "vk_loop_in_data_if.frag", "vk_loop_in_uniform_if.frag",
           "vk_loop_in_unrolled.frag", "vk_vertex_loop_nest.vert", "vk_loop_nest_grid.frag", "vk_loop_step_grid.frag",
           "vk_loop_sample_lod.frag", "vk_loop_sample_grad.frag", "vk_loop_sample_computed.frag",
           "vk_loop_derivative.frag", "vk_loop_parallax.frag", "vk_vertex_loop_sample.vert", "vk_loop_sample_grid.frag",
           "vk_scalar_input_loop.frag", "vk_scalar_input.frag", "vk_vertex_scalar_input.vert",
           "vk_instance_cells.vert", "vk_instance_step.vert", "vk_indexed_reads.vert", "hlsl/grid_vs.hlsl",
           "hlsl/grid_link_ps.hlsl", "hlsl/grid_vpos_ps.hlsl", "hlsl/grid_vface_ps.hlsl", "hlsl/grid_depth_ps.hlsl",
           "hlsl/grid_gaps_vs.hlsl", "hlsl/grid_gaps_ps.hlsl")
# xsdk's split (xsdk:src/gpu_setup.c).
REGISTERS = 64
RESOURCE_KINDS = {"push": "XSDK_XSC_PUSH_CONSTANTS", "uniform": "XSDK_XSC_UNIFORM_BUFFER",
                  "sampler": "XSDK_XSC_SAMPLER", "instance": "XSDK_XSC_INSTANCE"}


def print_words(name, words):
    print(f"static const uint32_t {name}[] = {{")
    for word in words:
        print(f"\t0x{word:08X},")
    print("};\n")


def main():
    print("#ifndef XSDK_XSC_SHADERS_H\n#define XSDK_XSC_SHADERS_H\n\n#include <stddef.h>\n#include <stdint.h>\n")
    print('#include "xsdk/xsc.h"\n')
    print("// Generated by tools/gen_xsc_shaders.py.\n")
    print("typedef struct {\n\tconst char *name;\n\tconst uint32_t *spirv;\n\tsize_t spirv_words;\n"
          "\tconst uint32_t *container;\n\tsize_t container_words;\n\tconst XsdkXscNeed *needs;\n\tsize_t need_count;\n"
          "\tconst XsdkXscResource *resources;\n\tsize_t resource_count;\n} XscTestShader;\n")
    table = []
    for i, name in enumerate(SHADERS):
        blob = (TESTDATA / f"{name}.spv").read_bytes()
        container, manifest, resources = native.compile_spirv(blob, REGISTERS, REGISTERS, optimize=True)
        print_words(f"xsc_spirv_{i}", struct.unpack(f"<{len(blob) // 4}I", blob))
        print_words(f"xsc_container_{i}", struct.unpack(f">{len(container) // 4}I", container))
        if manifest:
            print(f"static const XsdkXscNeed xsc_needs_{i}[] = {{")
            for need in manifest:
                if need[0] == "block_loop":
                    _, reg, kind, s, b, offset, start, step = need
                    print(f"	{{ .kind = XSDK_XSC_BLOCK_LOOP, .reg = {reg}, .regs = 1, .block = {RESOURCE_KINDS[kind]}, "
                          f".set = {s}, .binding = {b}, .offset_bytes = {offset}, .start = {start}, .step = {step} }},")
                    continue
                if need[0] == "fixed_loop":
                    print(f"	{{ .kind = XSDK_XSC_FIXED_LOOP, .reg = {need[1]}, .regs = {need[2]} }},")
                    continue
                if need[0] == "clip":
                    print(f"	{{ .kind = XSDK_XSC_CLIP_PLANE, .reg = {need[2]}, .regs = {need[1]} }},")
                    continue
                reg, values = need
                print(f"	{{ XSDK_XSC_LITERAL, {reg}, 1, {{ {', '.join(repr(v) for v in values)} }}, NULL, "
                      f"XSDK_XSC_PUSH_CONSTANTS, 0, 0, 0, 0, 0 }},")
            print("};\n")
        if resources:
            print(f"static const XsdkXscResource xsc_resources_{i}[] = {{")
            for kind, s, b, reg, regs in resources:
                print(f"\t{{ {RESOURCE_KINDS[kind]}, {s}, {b}, {reg}, {regs} }},")
            print("};\n")
        table.append(f'\t{{ "{name}", xsc_spirv_{i}, {len(blob) // 4}, xsc_container_{i}, {len(container) // 4}, '
                     f"{f'xsc_needs_{i}' if manifest else 'NULL'}, {len(manifest)}, "
                     f"{f'xsc_resources_{i}' if resources else 'NULL'}, {len(resources)} }},")
    print("static const XscTestShader xsc_test_shaders[] = {")
    print("\n".join(table))
    print("};\n\n#endif")


if __name__ == "__main__":
    main()
