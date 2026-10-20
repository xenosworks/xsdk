#!/usr/bin/env python3
"""Compile a title's GLSL and HLSL shaders into one C header, for xsdk_shader_load (docs/GUIDE.md, "Shaders and their
constants").

Each .vert or .frag goes through glslang to Vulkan SPIR-V, and each <name>_vs.hlsl or <name>_ps.hlsl, shader model 3,
through glslang with the flags xenos-toolchain/docs/xsc.md, "HLSL", gives; then each through xost's compiler at xsdk's
register split with the optimiser on (xenos-toolchain/docs/xsc.md). For each shader the header holds:
- its container and stage, as an XsdkBuiltShader named after the file: triangle.vert gives triangle_vert, and
  quad_ps.hlsl quad_ps_hlsl;
- its literal constants, which xsdk_built_shader_set_literals sets after xsdk_gpu_set_shaders;
- where its push constants, uniform blocks and samplers landed, as an enum: TRIANGLE_VERT_PUSH, the first float
  constant of the push constants, and TRIANGLE_VERT_PUSH_REGISTERS, how many; TRIANGLE_FRAG_SAMPLER_0_0, the texture
  fetch constant of the sampler at set 0, binding 0; and the same for uniform blocks and comparisons. An HLSL sampler's
  binding is its s#; a title finds HLSL's constants by name (xsdk_shader_constant).

    build_shaders.py --output build/triangle_shaders.h triangle.vert triangle.frag

Runs in WSL, after make has built xost's compiler for the PC (build/host/libxsc.so). glslang is --glslang, else the one
on PATH, else, for GLSL alone, xost's Windows build in xenos-toolchain/tools/glslang, which WSL runs. HLSL needs
glslang-xenon (docs/GETTING_STARTED.md, section 4), whose fmod truncates and which keeps b# and i#; nothing here tells
it from Khronos's glslang, which reports the same version.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402

WINDOWS_GLSLANG = ROOT / "xenos-toolchain" / "tools" / "glslang" / "bin" / "glslang.exe"
# Each kind of source by the end of its name, and its stage.
STAGES = {".vert": "XSDK_SHADER_VERTEX", ".frag": "XSDK_SHADER_PIXEL", "_vs.hlsl": "XSDK_SHADER_VERTEX",
          "_ps.hlsl": "XSDK_SHADER_PIXEL"}
# HLSL's sampler2D a texture and sampler in one, and each varying's semantic and a bool's b# and an int's i# kept.
HLSL_FLAGS = ["-D", "-V", "--hlsl-dx9-compatible", "-fhlsl_functionality1"]
HLSL_ENTRY = re.compile(r"\b(vs_main|ps_main)\s*\(")
WORDS_PER_LINE = 6

# The types and the literal setter, once however many generated headers a file includes.
COMMON = """#ifndef XSDK_BUILT_SHADER
#define XSDK_BUILT_SHADER
// A float constant a shader reads a literal from (xenos-toolchain/docs/xsc.md, "Literal constant pool").
typedef struct {
\tuint32_t reg;
\tfloat values[4];
} XsdkShaderLiteral;

// A shader's container for xsdk_shader_load, its stage, and the literals its stage's constants hold while it draws.
typedef struct {
\tconst uint32_t *container;
\tuint32_t container_bytes;
\tuint32_t type;
\tconst XsdkShaderLiteral *literals;
\tuint32_t literal_count;
} XsdkBuiltShader;

// Sets the shader's literals for the draws after; call it after each xsdk_gpu_set_shaders.
static inline void xsdk_built_shader_set_literals(const XsdkBuiltShader *shader)
{
\tfor (uint32_t i = 0; i < shader->literal_count; i++) {
\t\tconst XsdkShaderLiteral *const literal = &shader->literals[i];
\t\tif (shader->type == XSDK_SHADER_VERTEX)
\t\t\txsdk_gpu_set_vertex_constants(literal->reg, literal->values, 1);
\t\telse
\t\t\txsdk_gpu_set_pixel_constants(literal->reg, literal->values, 1);
\t}
}
#endif
"""


def find_glslang(given, hlsl):
    # Whole, as glslang runs in another directory.
    found = shutil.which(given or "glslang")
    if found:
        return Path(found).absolute()
    # The Windows build is Khronos's, which gets HLSL's fmod and registers wrong.
    if hlsl:
        sys.exit("no glslang for HLSL; pass --glslang, or build glslang-xenon as docs/GETTING_STARTED.md, section 4, "
                 "says")
    if not given and WINDOWS_GLSLANG.exists():
        return WINDOWS_GLSLANG
    sys.exit("build_shaders.py: no glslang; pass --glslang, or see docs/GETTING_STARTED.md, \"glslang\"")


def kind_of(source):
    """The STAGES key the source's name ends in, or None."""
    return next((k for k in STAGES if source.name.endswith(k)), None)


def hlsl_args(source):
    """glslang's arguments for HLSL: the stage from the name, and the entry vs_main or ps_main where there is one."""
    stage = "vert" if source.stem.endswith("_vs") else "frag"
    found = HLSL_ENTRY.search(source.read_text())
    return HLSL_FLAGS + ["-S", stage, "-e", found.group(1) if found else "main"]


def spirv_of(glslang, source, directory):
    # A copy run by its bare name, so a Windows glslang run from WSL finds it as well.
    (directory / source.name).write_bytes(source.read_bytes())
    args = hlsl_args(source) if source.suffix == ".hlsl" else ["-V"]
    result = subprocess.run([str(glslang), *args, source.name, "-o", "shader.spv"], cwd=directory, capture_output=True,
                            text=True)
    if result.returncode != 0:
        sys.exit(f"glslang failed on {source}:\n{result.stdout}{result.stderr}")
    return (directory / "shader.spv").read_bytes()


def compile_shader(spirv, source):
    try:
        return native.compile_spirv(spirv, XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, optimize=True)
    except native.CompileError as error:
        sys.exit(f"{source}: xost didn't compile it ({native.STATUS_NAMES[error.status]}): {error}")
    except OSError as error:
        sys.exit(f"build_shaders.py: xost's compiler didn't load; run make in xsdk first ({error})")


def shader_lines(name, source, container, manifest, resources):
    literals = []
    for need in manifest:
        # An HLSL loop's int is the loop constant its table names, which the title sets by name.
        if need[0] == "block_loop" and source.suffix == ".hlsl":
            continue
        # Loops the runtime counts and clip planes need more than a literal from the title.
        if len(need) != 2:
            sys.exit(f"{source}: xost wants a constant build_shaders.py doesn't write yet: {need!r}")
        literals.append(need)
    words = struct.unpack(f">{len(container) // 4}I", container)
    lines = [f"static const uint32_t {name}_container[] = {{"]
    for i in range(0, len(words), WORDS_PER_LINE):
        lines.append("\t" + ", ".join(f"0x{word:08X}" for word in words[i:i + WORDS_PER_LINE]) + ",")
    lines += ["};", ""]
    literal_table = "NULL"
    if literals:
        literal_table = f"{name}_literals"
        lines.append(f"static const XsdkShaderLiteral {literal_table}[] = {{")
        for reg, values in literals:
            lines.append(f"\t{{ {reg}, {{ {', '.join(repr(float(v)) + 'f' for v in values)} }} }},")
        lines += ["};", ""]
    lines += [f"static const XsdkBuiltShader {name} = {{", f"\t{name}_container, sizeof {name}_container, "
              f"{STAGES[kind_of(source)]}, {literal_table}, {len(literals)},", "};", ""]
    if resources:
        lines += [f"// Where {source.name}'s resources landed: float constants, or a sampler's texture fetch constant.",
                  "enum {"]
        for kind, dset, binding, reg, regs in resources:
            enum = f"{name.upper()}_{kind.upper()}" + ("" if kind in ("push", "instance") else f"_{dset}_{binding}")
            lines.append(f"\t{enum} = {reg},")
            if kind in ("push", "uniform"):
                lines.append(f"\t{enum}_REGISTERS = {regs},")
        lines += ["};", ""]
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output", type=Path, required=True, help="the header to write")
    parser.add_argument("--glslang", help="glslang, by default the one on PATH, then for GLSL xost's Windows build")
    parser.add_argument("sources", type=Path, nargs="+", help=".vert, .frag, _vs.hlsl and _ps.hlsl files")
    args = parser.parse_args()
    glslang = find_glslang(args.glslang, any(source.suffix == ".hlsl" for source in args.sources))
    # Sorted, so the header is the same however the build lists them.
    sources = sorted(args.sources, key=lambda path: path.name)
    names = [re.sub(r"\W", "_", source.name) for source in sources]
    if len(set(names)) != len(names):
        sys.exit(f"build_shaders.py: two shaders share a name: {names}")
    guard = re.sub(r"\W", "_", args.output.name).upper()
    lines = [f"// Generated by tools/build_shaders.py from {', '.join(s.name for s in sources)}; don't edit.",
             f"#ifndef {guard}", f"#define {guard}", "", "#include <stdint.h>", "", '#include "xsdk/gpu.h"',
             '#include "xsdk/shader.h"', "", COMMON]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Beside the output, so a Windows glslang can reach it from WSL whatever the build directory.
    with tempfile.TemporaryDirectory(dir=args.output.parent) as directory:
        for name, source in zip(names, sources):
            if not kind_of(source):
                sys.exit(f"{source}: not a .vert, .frag, _vs.hlsl or _ps.hlsl")
            container, manifest, resources = compile_shader(spirv_of(glslang, source, Path(directory)), source)
            lines += shader_lines(name, source, container, manifest, resources)
    lines.append("#endif")
    args.output.write_text("\n".join(lines) + "\n", newline="\n")


if __name__ == "__main__":
    main()
