# Milestone 50: HLSL

Agreed on 2026-10-19, to run beside milestone 48 and the others agreed that day.

## Why

PC ports of the D3D9 era bring their shaders as HLSL for shader model 3 (vs_3_0, ps_3_0). xost takes SPIR-V, and
glslang 16.3.0, the one xost is checked with, reads HLSL. So the path is HLSL through glslang to SPIR-V, then xost,
and a port keeps its shaders as they are.

A trial on 2026-10-19 used 157 SM3 shaders that SPIRV-Cross wrote from Dusklight's and vkbench's GLSL, and 21 written
by hand for D3D9's idioms. Flags: `glslang -D -V --hlsl-dx9-compatible -S <stage> -e <entry>`; xost at xsdk's
split, optimised.
- **glslang read all 172.** Without `--hlsl-dx9-compatible`, `sampler2D` becomes a bare sampler.
- **xost compiled 0 of the 157 and 9 of the 21.** Prototype fixes (kept out of the repository) took the 157 to 157 and
  the 21 to 13. Then 123 of the 157 matched the GLSL path's instruction counts exactly, 4395 ALU against 4364.
- **xsdk already rasterises as D3D9 does:**
  - pixel centres on whole coordinates under `PIX_CENTER_D3D` (src/gpu_setup.c; docs/HARDWARE.md, "Triangle fans, and
    raster facts through vulkan4xenon");
  - z from 0 to w (src/gpu_draw.c);
  - the viewport's y up.

  A port keeps its half-texel offsets and sets its viewport as on D3D9.
- **Registers:** glslang puts the globals in one `$Global` block at `register(cN)`'s offsets, which xost lays from c0.
  So HLSL's cN is cN for `xsdk_gpu_set_*_constants`, as `SetVertexShaderConstantF` sets them.

What stopped xost, by how often:
1. **`static` globals** (Private storage): only Function variables are locals (src/xsc_lower_stage.c:36-46). 105 of
   the pixel shaders.
2. **Splat constants** that spirv-opt folds to `OpConstantComposite`: `xsdk_xsc_const_scalar` takes only
   `OpCompositeConstruct` (src/xsc_lower.c:629-653). 66 shaders.
3. **D3D9's semantics.** glslang maps POSITION, PSIZE, COLORn and DEPTH only on `out` parameters; members and return
   values become locations in declaration order. VPOS and VFACE then compile, wrongly, as interpolators. The vertex
   inputs' usages come from location order, not their semantics (src/xsc_resolve.c:348-354).
4. **Loops in SSA form.** glslang always runs spirv-opt on HLSL, so loops arrive as phis with the test in the header,
   where xost's unroller takes GLSL's form in memory (src/xsc_unroll.c:1738).
5. **A float2 VPOS:** any whole-vector use of FragCoord counts as reading z and w (src/xsc_lower_fragment.c:68-110).
6. **Behind those:**
   - matrices that aren't loaded from uniforms (a `(float3x3)` cast, a varying TBN);
   - constant arrays indexed by a loop's counter;
   - bool constants;
   - int vectors (skinning indices);
   - a lone PSIZE;
   - separate `Texture2D` and `SamplerState`.

And one in glslang: HLSL's `fmod` becomes `OpFMod`, which floors where HLSL truncates (glslang
hlslParseables.cpp:1074).

## What it takes

1. **The harness.**
   - The HLSL corpus and the D3D9 idiom set go in xost's testdata.
   - `tools/check_hlsl.py` runs each through glslang and xost and sorts the refusals by cause. Its counts go in
     `results/`.
   - On xdis/simulate.py, each shader's results are compared with the GLSL path's for the same source.
2. **xost's front:** Private variables as locals, `OpConstantComposite` splats, `OpUndef` and `OpConstantNull`, and a
   float2 FragCoord.
3. **Loops in SSA form.** A pass before lowering puts phis into memory, and loops into the shape the unroller takes.
   Nested loops, breaks and merged continue blocks make this the milestone's main risk.
4. **D3D9's semantics,** from `-fhlsl_functionality1`'s semantic strings:
   - POSITION, COLORn, DEPTH and PSIZE as built-ins;
   - VPOS as FragCoord less 0.5, keeping c255 for tiles;
   - VFACE as ±1, which face first settled on the console;
   - vertex inputs' usages from their semantics;
   - a vertex shader's outputs linked to a pixel shader's inputs by semantic, as xsdk links a vertex layout.
5. **The rest, by how often the corpus meets it:** constant arrays in loops, matrices that aren't uniforms, bool
   constants, int vectors, a lone PSIZE.
6. **Binding as D3D9 binds:**
   - a sampler at its `s#` slot, not packed from 0;
   - bool and int constants out of `$Global`, where they overlap c0 today;
   - the container's constant table with each constant's components, so a port can bind by name.
7. **`fmod`:** glslang-xenon emits HLSL's truncating form. The bug is reported to glslang as well.
8. **In a title's project:** `tools/build_shaders.py` (milestone 41's) takes `.hlsl` files with the fixed flags, and
   xenos-toolchain/docs/xsc.md gains an HLSL section, with what xost refuses.

Left out:
- shader model 4 and later;
- D3D9's assembly shaders;
- Effects files (`.fx`) and their techniques. A port that uses Effects compiles each technique's entry points
  itself.

## Verification

- **On the PC:**
  - xost's tests pass;
  - check_hlsl.py's counts against the baseline;
  - the corpus's results on the simulator equal the GLSL path's;
  - the idioms' results equal what HLSL defines.
- **On the console:** an xsdk_test HLSL grid, each check with output of its own:
  - transforms under row-major and column-major matrices;
  - samplers at their `s#`;
  - VPOS and VFACE;
  - `clip`;
  - four targets with depth;
  - a loop;
  - skinning from int indices.

  Each check is broken on purpose once. vulkan4xenon_test runs twice, unchanged.

## Done when

- All 157 of the corpus and all 21 idioms compile, or each refusal is a construct the docs name.
- The corpus's results on the simulator equal the GLSL path's.
- The console's HLSL grid passes, its breaks failing the checks they name.
- A title's project builds `.hlsl` shaders with one call, and binds their constants and samplers as on D3D9.
