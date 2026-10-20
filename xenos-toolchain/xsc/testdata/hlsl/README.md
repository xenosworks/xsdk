# HLSL testdata

Shader model 3 HLSL for xsdk milestone 50 (xsdk:docs/scope/50-hlsl.md), each `.hlsl` with the SPIR-V glslang 16.3.0
makes of it, `glslang -D -V --hlsl-dx9-compatible -fhlsl_functionality1 -S <stage> -e <entry>`, as
`tools/check_hlsl.py --refresh` writes it; the last flag keeps each varying's semantic for xost.

- **`corpus/`**: 157 shaders SPIRV-Cross (Debian's 2021.01.15 package) wrote at shader model 30 from Dusklight's 150 GX
  shaders and vkbench's 7, with D3D9's half-pixel lines dropped, each cbuffer flattened into uniforms at registers of
  their own, the blocks one after another, and `discard` written as `clip(-1.0f)`. Beside each, `<name>_ps.hlsl` or
  `<name>_vs.hlsl`, is its GLSL twin, `<name>.frag` or `<name>.vert`, as at dusklight-xenon 583647ae67
  (`platforms/xenon/shaders`) and vulkan4xenon 1cb5ac4 (`bench/shaders`), with the SPIR-V `glslang -V` makes of it,
  which SPIRV-Cross read.
- **`idioms/`**: 21 written by hand for D3D9's idioms, the trial's of 2026-10-19: transforms, samplers, VPOS and VFACE,
  intrinsics, half, branches, depth and four targets, skinning, bool and int constants, lighting, DX10 semantics,
  cbuffers, a post-process pass, and where a matrix's majorness and a sampler's register put them.
- **`static_globals_ps`, `splat_clamp_ps`, `undefined_ps` and `float2_position_ps`**: `tests/test_hlsl_front.py`'s, one
  for each of milestone 50 step 2's changes to xost's front.
- **`semantics_*`, `vpos_*` and `refused_*`**: `tests/test_hlsl_semantics.py`'s, D3D9's semantics, step 4.
- **`loops/`**: `tests/test_hlsl_loops.py`'s, loops in SSA form, step 3, each `<name>_ps.hlsl` beside its GLSL twin
  `<name>.frag` with the SPIR-V `glslang -V` makes of it, and three refusals, `refused_*`.
- **`rest/`**: `tests/test_hlsl_rest.py`'s, step 5's constructs: constant arrays, matrices built of columns and
  matrix varyings, bools, ints, vector transcendentals and a lone PSIZE, each `<name>_ps.hlsl` or `<name>_vs.hlsl`
  beside its GLSL twin where one is written, `grid_skin.vert` `grid_skin_vs.hlsl`'s, and two refusals, `refused_*`.
- **`binding/`**: `tests/test_hlsl_binding.py`'s, binding as D3D9 binds, step 6: samplers at their `s#`
  (`slots_ps.hlsl`, beside its GLSL twin `slots.frag`), bools and ints out of the float registers glslang packs them
  into (`integers_ps.hlsl`, `unrolled_ps.hlsl`), the constant table's entries (`table_ps.hlsl`) and a refusal,
  `refused_step_ps.hlsl`.
- **`grid_*`**: xsdk_test's HLSL grid (xsdk:tests/test_hlsl.c), compiled on the console from this SPIR-V;
  `tests/test_hlsl_link.py` links `grid_gaps_*` on the PC, `tests/test_hlsl_rest.py` runs `grid_skin_vs` and
  `tests/test_hlsl_binding.py` `grid_binding_ps`.
