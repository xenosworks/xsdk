# Milestone 50: HLSL

What xost makes of shader model 3 HLSL through glslang (docs/scope/50-hlsl.md), counted by
`xenos-toolchain/tools/check_hlsl.py` over xost's HLSL testdata (`xenos-toolchain/xsc/testdata/hlsl`, its README says
where each set came from): the corpus, 157 shaders SPIRV-Cross wrote from Dusklight's and vkbench's GLSL, 107 pixel
and 50 vertex, and the 21 idioms. Each `.json` holds every shader's result and the totals; each `.txt` is what the run
printed.

## How it was made

In WSL Debian, after `make clean` and `make -j32` had built `build/host/libxsc.so`, from `xenos-toolchain/`:

```
python3 tools/check_hlsl.py --json ../results/2026-10-20-milestone-50/<run>.json > ../results/2026-10-20-milestone-50/<run>.txt
```

glslang is xost's Windows build of 16.3.0, `-D -V --hlsl-dx9-compatible -S <stage> -e <entry>`; xost compiles at
xsdk's split, 64 and 64, with the optimiser on. A refusal's cause is read from xost's message and, where it names an
instruction, from what that instruction is: an `unresolved Load` of a Private variable is a static global. A pixel
shader that compiles but declares VPOS or VFACE is counted apart: glslang makes them locations, so xost reads them as
interpolators, which step 4 corrects.

Each corpus pixel shader that compiles runs on `xdis/simulate.py` beside its GLSL twin compiled as GLSL is today, 20
trials, on the same random interpolators, constants (HLSL's cN at cN, the GLSL's in its blocks, matched by name) and
textures (a smooth function of the coordinates and LOD, one for each sampler's name). Exact means every export
component the output's type has is the same bit for bit in every trial. Vertex shaders aren't compared yet: none
compiles before step 4, which matches their fetches by input.

## Baseline: xost at xsdk 70b2f2f

`baseline.txt`, `baseline.json`.

| | Corpus, 157 | Idioms, 21 |
|---|---|---|
| Compiled | 0 | 9 |
| A static global (Private variable) | 107 | 0 |
| D3D9 semantics: POSITION out a location (step 4) | 50 | 3 |
| A loop in SSA form (step 3) | 0 | 5 |
| A splat constant (OpConstantComposite) | 0 | 1 |
| D3D9 semantics: inputs, VPOS or VFACE among them, as locations (step 4) | 0 | 1 |
| D3D9 semantics: DEPTH out a colour's location (step 4) | 0 | 1 |
| An int or bool constant (step 5) | 0 | 1 |

Against the GLSL path: none comparable, as none compiles. The idioms that compile are `i03` (samplers), `i06` (half),
`i12` and `i13` (DX10 semantics), `i14` (a cbuffer) and `m1` to `m4` (matrices' majorness and samplers' registers).

## Step 2: xost's front

`step2.txt`, `step2.json` (docs/scope/50-hlsl.md, step 2):
- **Static globals** (src/xsc_spirv.c, `xsdk_xsc_declares_local`): a Private variable is a local wherever xost finds
  locals, the lowering's (src/xsc_lower_stage.c, `xsdk_xsc_local_vars`) and the unroller's (src/xsc_unroll.c,
  `find_locals`, `stores_locals_again`), as each invocation holds its own (SPIR-V 1.0, section 3.7). Without it, all
  107 corpus pixel shaders stay refused.
- **Splats** (src/xsc_lower.c, `xsdk_xsc_const_scalar`): clamp's bounds as an `OpConstantComposite` of one constant, as
  glslang folds `clamp(v, -4.0, 4.0)`, beside an `OpCompositeConstruct` of one. Without it, 66 corpus shaders and the
  idiom `i05` stay refused.
- **OpUndef and OpConstantNull** (src/xsc_spirv.c, `xsdk_xsc_spirv_zero_nulls`, run after parsing): of a 32-bit scalar
  or a vector of them, before the functions, a constant zero, a vector's lanes one zero minted for its component type;
  an undefined value may be any (SPIR-V 1.0, section 3.32.1) and a null one is zero (section 3.32.7). Without it, 7
  corpus shaders stay refused.
- **A float2 FragCoord** (src/xsc_lower_fragment.c, `reads_depth`): one of two components has no z or w, so its whole
  use no longer counts as reading them. No shader of the corpus or the idioms has one: HLSL's `float2 pos :
  SV_Position` does, and step 4's VPOS will.

| | Corpus, 157 | Idioms, 21 |
|---|---|---|
| Compiled | 82 | 10 |
| Compiled, VPOS read as an interpolator (step 4) | 25 | 0 |
| D3D9 semantics: POSITION out a location (step 4) | 50 | 3 |
| A loop in SSA form (step 3) | 0 | 5 |
| D3D9 semantics: inputs, VPOS or VFACE among them, as locations (step 4) | 0 | 1 |
| D3D9 semantics: DEPTH out a colour's location (step 4) | 0 | 1 |
| An int or bool constant (step 5) | 0 | 1 |

**Against the GLSL path:** 82 exact, 0 inexact, 75 not comparable: the 50 vertex shaders, refused, and the 25 that
read VPOS. One of the 82, `6a05b1f88a016fb2_ps`, exports a constant, and glslang drops its input, which the GLSL
keeps. Broken on purpose, each name of the HLSL's constants, samplers or interpolators made apart from the GLSL's
(3 trials), 43, 72 and 25 of the 82 went inexact; those left are shaders without constants, without samplers, or
with one interpolator.

**xost's tests:** every `tests/test_*.py` but `test_xcomb_parity.py` passes; `test_scalar_unit.py` with its corpus at
`/mnt/z/vulkan4xenon`, as this worktree has no vulkan4xenon beside it. `tests/test_hlsl_front.py` runs a shader for
each change, `xsc/testdata/hlsl/static_globals_ps.hlsl`, `splat_clamp_ps.hlsl`, `undefined_ps.hlsl` and
`float2_position_ps.hlsl`, on the simulator, the optimiser off and on, every export exact; its static global stored
twice takes the unroller. Each change broken on purpose failed it: without static globals `unresolved Load`, and
without them in the unroller's `find_locals` or `stores_locals_again` alone `stored more than once where the unroller
doesn't rewrite it`; without splats `%18 is not a constant scalar`; without the zeros `unhandled op Op1`, and with a
null made 1.0 on its exports; without the float2 `gl_FragCoord.zw`.

**GLSL unchanged:** every GLSL program of xost's testdata (205), vulkan4xenon's `tests/shaders`, `src/shaders` and
`bench/shaders` (95) and Dusklight's pack sources (150), the optimiser off and on, 900 programs, compiled by 70b2f2f
and by this step: 870 containers, manifests and resources the same byte for byte, and the 30 refused refused alike.
`tests/xsc_shaders.h` regenerates as it is.
