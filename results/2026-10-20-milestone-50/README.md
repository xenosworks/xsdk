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
