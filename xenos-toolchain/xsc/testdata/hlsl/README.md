# HLSL testdata

Shader model 3 HLSL for xsdk milestone 50 (xsdk:docs/scope/50-hlsl.md), each `.hlsl` with the SPIR-V glslang 16.3.0
makes of it, `glslang -D -V --hlsl-dx9-compatible -fhlsl_functionality1 -S <stage> -e <entry>`, as
`tools/check_hlsl.py --refresh` writes it; the last flag keeps each varying's semantic for xost. Since step 7 it is
glslang-xenon's, xsdk's glslang on 16.3.0, which makes the same SPIR-V but where HLSL's `fmod` truncates and where a
bool or int keeps its `b#` or `i#`: `idioms/i05` and `i10`, `binding/`'s `integers_ps`, `unrolled_ps` and
`refused_step_ps`, `grid_binding_ps`, `fmod/` and `registers/`.

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
- **`fmod/`**: `tests/test_hlsl_fmod.py`'s, HLSL's truncating `fmod`, step 7: a vector's lanes and scalars.
- **`registers/`**: `tests/test_hlsl_registers.py`'s, D3D9's `b#` and `i#`, step 7: bools and ints at their registers
  beside ones without (`registers_ps.hlsl`, `registers_vs.hlsl`) and three refusals, `refused_*`.
- **`if/`**: `tests/test_hlsl_if.py`'s, an if/else changing a value in a pixel shader without a loop, step 8: an if
  with no else (`if_only_ps.hlsl`, beside its GLSL twin `if_only.frag`), an else alone, nested if/else, an if on a
  sample and one on a bool at `register(b2)`.
- **`return/`**: `tests/test_hlsl_return.py`'s, early returns, which spirv-opt's merge-return wraps in a switch, step 8:
  under an if (`if_ps.hlsl`, beside its GLSL twin `if.frag`), in the else, in an if inside another, after a sample,
  two of them, in a vertex shader, in an unrolled loop and in a hardware loop, and a refusal, a discard after a return
  on data (`refused_discard_ps.hlsl`, beside its GLSL twin `refused_discard.frag`).
- **`switch/`**: `tests/test_hlsl_return.py`'s, switches, step 8: cases that break, two literals sharing one, and a
  default (`switch_ps.hlsl`, beside its GLSL twin `switch.frag`), a vertex shader's without a default, an early return
  in a case, and a refusal, a case falling through to the next.
- **`grid_*`**: xsdk_test's HLSL grid (xsdk:tests/test_hlsl.c), compiled on the console from this SPIR-V;
  `tests/test_hlsl_link.py` links `grid_gaps_*` on the PC, `tests/test_hlsl_rest.py` runs `grid_skin_vs`,
  `tests/test_hlsl_binding.py` `grid_binding_ps`, `tests/test_hlsl_if.py` `grid_if_ps` and `tests/test_hlsl_return.py`
  `grid_return_ps` and `grid_switch_ps`.
