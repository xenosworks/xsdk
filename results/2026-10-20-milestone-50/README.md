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

## Step 4: D3D9's semantics

`step4.txt`, `step4.json` (docs/scope/50-hlsl.md, step 4). check_hlsl.py now passes glslang `-fhlsl_functionality1`,
which keeps each varying's semantic as an OpDecorateString of UserSemantic; the counts with it alone, before xost read
them, were step 2's. xost reads them where glslang left a location (src/xsc_resolve.c; xenos-toolchain/docs/xsc.md,
"Interface conventions"):
- **A vertex input's usage and index** are its semantic's, D3D9's usages as libxenon numbers them; any other is refused.
- **POSITION, PSIZE, DEPTH and COLORn outputs** are the position, the point's size, the depth and target n, struct
  members and return values too.
- **VPOS** is the pixel parameters' |xy| plus c255's zw, which xsdk now keeps at 0 and a tiled band's top row
  (src/gpu_constants.c): gl_FragCoord less 0.5, the pixel's whole column and row. A float4 VPOS, as SPIRV-Cross
  declares the corpus's, reads 0 in z and w.
- **VFACE** is +1 on a front face and -1 on a back one, from the sign gl_FrontFacing reads, one constant
  (`VFACE_FRONT`, src/xsc_lower_fragment.c) until xsdk_test's `hlsl: VFACE` check settles which sign a front face gets.
- **Linking:** each semantic has one interpolator in both stages, TEXCOORD0 at 0, COLOR0 at 1, TEXCOORD1 to 11 at 2
  to 12, COLOR1 at 13 and FOG at 14, r15 left for the pixel parameters. The runtime does nothing new: a pixel shader
  declares the interpolators below its highest that it doesn't read, and a vertex shader's export count runs to its
  highest. A gap costs an interpolator; a sprite's TEXCOORD0 and COLOR0 take two.
- **The guard:** a Function or Private variable with an initializer, which xost would drop, is refused
  (src/xsc_compile.c). Nothing here has one; glslang writes a static's value as a store.

| | Corpus, 157 | Idioms, 21 |
|---|---|---|
| Compiled | 157 | 13 |
| A loop in SSA form (step 3) | 0 | 5 |
| A matrix not loaded from uniforms (step 5) | 0 | 1 |
| An int vector (step 5) | 0 | 1 |
| An int or bool constant (step 5) | 0 | 1 |

Against step 2: the corpus's 50 vertex shaders and 25 VPOS pixel shaders compile, and in the idioms `i02` (FOG and
PSIZE), `i04` (VPOS and VFACE) and `i08` (DEPTH and four targets). `i01` and `i09` compile past POSITION now and stop at
step 5's causes, a `(float3x3)` cast and `D3DCOLORtoUBYTE4`'s int4.

**Against the GLSL path:** 151 exact, 6 inexact, every shader comparable. The vertex shaders are compared now: each
input's fetch is told apart by its element's input, every input the same random values from 0 to 2, so an index a
shader works out from one stays in its array, as GX's matrix indices do; the position and every interpolator are
compared. All 50 are exact. The 6 inexact are VPOS pixel shaders off by an ulp in alpha, at most 8.9e-8: their
`x * 255.0 / 256.0` is one multiply by 0.99609375 in glslang's HLSL, which its optimiser folds, and two in the GLSL.
check_hlsl.py now rounds its random constants and inputs to singles, as the GPU holds them; with doubles, a constant
read straight into a multiply made 1-ulp differences the console can't. Broken on purpose, the HLSL's vertex inputs
given values apart from the GLSL's (3 trials), all 50 vertex shaders went inexact.

**xost's tests:** every `tests/test_*.py` but `test_xcomb_parity.py` passes, `test_scalar_unit.py` with its corpus at
`/mnt/z/vulkan4xenon`. `tests/test_hlsl_semantics.py` runs `semantics_vs.hlsl`, `semantics_ps.hlsl`, the two linked
(the vertex shader's exports as the pixel shader's registers), `vpos_vface_ps.hlsl` (columns 3 and 0 of each face, in
a band from row 0 and 64), `vpos_wide_ps.hlsl` and three refusals, the optimiser off and on; `tests/test_hlsl_front.py`
the guard. Each change broken on purpose failed them: usages by location `KeyError: (5, 5)`; POSITION not read
`interpolators link`; PSIZE not exported `export 63 None`; inputs by location `export 0 [1.0, 0.5, 0.25, 0.75]` (and
test_hlsl_front.py); outputs by location `export 1 [0.125, ...]`; COLORn by location `export 0 [1.5, 4.5, -6.0,
0.75]`; DEPTH not read `two colour outputs at location 1`; VPOS from c255's xy `[3.5, 7.5, ...]`; a float4 VPOS's z
and w not cleared `[5.0, 9.0, 5.0, 9.0]`; VFACE_FRONT -1 `[3.0, 7.0, -1.0, ...]`; the gaps not declared `declare
inputs in r0,r1,... order`; the export count by exports `5 interpolators exported`; a shared semantic not refused
`declare inputs in r0,r1,... order`; OpDecorateString not read `no write to gl_Position`; the guard off `a Private
variable with an initializer compiled`.

**GLSL unchanged:** the 900 programs of step 2, compiled by 5047f51 and by this step: the same containers, manifests
and resources byte for byte, and the 30 refused refused alike.

## Step 4, linked at bind

`step4b.txt`, `step4b.json`. Step 4 first gave each semantic one interpolator in both stages, so a pixel shader declared
every interpolator below its highest: one reading COLOR1 or FOG declared 14 or 15. D3D9's link replaces it
(docs/scope/50-hlsl.md, step 4; xenos-toolchain/docs/xsc.md, "Interface conventions"):
- **Packed by semantic** (src/xsc_resolve.c, `pack_semantics`): each stage takes interpolators from 0 in the order of
  its semantics, usage then index, so a pixel shader declares only those it reads and a vertex shader exports only
  those it writes. Any of D3D9's usages is an interpolator, where the map took TEXCOORD0 to 11, COLOR0, COLOR1 and FOG;
  a semantic that isn't one, two variables of one, or a varying without a semantic beside one with is refused. Each
  interpolator record holds its semantic, usage and index where XenosRecomp reads them, and bit 31 marks it HLSL's
  (src/xsc_container.c).
- **Linked when a title pairs them** (src/shader.c, `xsdk_shader_link`): each of the vertex shader's exports goes to
  the pixel shader's interpolator for its semantic; one the pixel shader doesn't read writes nothing, no write mask at
  interpolator 0, the form Xenia gives the XNA assembler's empty exports (xenia:src/xenia/gpu/ucode.h:2041-2059); and
  SQ_PROGRAM_CNTL's export count becomes the pixel shader's interpolators. It patches the vertex shader's microcode from
  its container's, as `xsdk_shader_set_vertex_layout` patches fetches, not while the vertex shader may run, and writes
  only words that change, so linking a pair again costs nothing. An input no output feeds is undefined. A GLSL pair is
  left alone, and one stage of each refused. The decisions are xost's `xsdk_xsc_link` and `xsdk_xsc_link_export`, which
  the PC's tests call through libxsc.so.
- **A call of its own,** not in `xsdk_gpu_set_shaders`: there it would have to know the GPU is done with the vertex
  shader, or copy it on the hot path, which allocates nothing. A vertex shader drawn with pixel shaders reading
  different semantics is loaded for each, as xsdk_test's grid now loads its vertex shader for each pixel shader.

| Corpus | Step 4's map | Packed |
|---|---|---|
| Interpolators of the 107 pixel shaders | 265, most 9 | 215, most 8 |
| Interpolators the 50 vertex shaders export | 189, most 9 | 149, most 8 |

50 of the pixel shaders and 40 of the vertex shaders take one fewer: SPIRV-Cross named the corpus's varyings TEXCOORD0
up from their locations, so the map's one gap was COLOR0's. Shaders using the other semantics show its cost: idiom
`i02_rowmajor_vs` exported 15 for its 3, FOG at 14, and `semantics_ps.hlsl` declared 15 for its 4.

**Against the GLSL path:** each shader's result is step 4's, 151 exact and 6 inexact, the same 6. check_hlsl.py
compares each stage alone, a varying at the interpolator xost gives it in that stage, so the link doesn't enter it.

**xost's tests:** every `tests/test_*.py` but `test_xcomb_parity.py` passes, `test_scalar_unit.py` with its corpus at
`/mnt/z/vulkan4xenon`. `tests/test_hlsl_link.py`, new, links xsdk_test's pair with gaps, `grid_gaps_vs.hlsl` (TEXCOORD3,
COLOR1, TEXCOORD0 and FOG) and `grid_gaps_ps.hlsl` (COLOR1 and TEXCOORD3), checks each record, every export word and
the export count after the link, runs the pair on the simulator linked and unlinked, checks that every export to an
interpolator in the 58 HLSL vertex shaders of the testdata sits at its container's export slots, each an ALU
instruction, and that a GLSL pair is left as it is and a mixed one refused; the optimiser off and on.
`tests/test_hlsl_semantics.py` checks the packed interpolators and links its pair through the same code. Each change
broken on purpose failed them: interpolators not packed `vector_dest=336 does not fit in 6 unsigned bits`; two of one
semantic not refused `refused_shared_ps.hlsl compiled`; records not marked `records ... not ..., marked` and `5
interpolators exported, not the pixel shader's 4`; exports not moved `slot 12's export 0xc80f8001 linked 0xc80f8001,
not 0xc80f8000`; unread exports left writing `slot 8's export 0xc80f8000 linked 0xc80f8000, not 0xc8008000`; the
vertex shader's count exported `4 exported, not 2`.

**xsdk_test:** `tests/test_hlsl.c` links each pixel shader to a vertex shader loaded for it, and adds a cell, the pair
with gaps, `hlsl: a vertex shader's TEXCOORD3, COLOR1, TEXCOORD0 and FOG meet a pixel shader's COLOR1 and TEXCOORD3 by
semantic, the other two unread: %u of %u pixels wrong`, and `hlsl: the pixel shader reading COLOR1 and TEXCOORD3
declares %u interpolators in its container, not 14`. `tests/breaks/hlsl/link.patch` now leaves each pair as loaded;
on the simulator the link's cell then draws red 191 and green 64 for 64 and 191, and the gaps' cell the ramp, 0 and 191
for 64, 191 and the ramp, failing both link checks.

**GLSL unchanged:** the 900 programs of step 2, compiled by 11a3e72 and by this step: the same containers, manifests
and resources byte for byte, and the 30 refused refused alike.

## Step 3: loops in SSA form

`step3.txt`, `step3.json` (docs/scope/50-hlsl.md, step 3). glslang runs spirv-opt's legalisation on HLSL, so each loop
arrives in SSA form: Phis and its test in its header, its counter's step in the block branching back, values read
across blocks. The unroller takes GLSL's memory form (src/xsc_unroll.c, `expand_loop`). A pass before lowering,
`xsdk_xsc_ssa_loops` (src/xsc_ssa.c, called in src/xsc_compile.c after the nulls are zeroed), puts such a module in that
form, and runs only where a loop's header holds more than a label, a merge and a branch, which glslang never writes
for GLSL (xenos-toolchain/docs/xsc.md, "Loops"):
- **Phis** become Function variables, each stored at the end of the block each edge leaves and loaded where it was; a
  Phi's value read in another block is loaded there again.
- **The header** keeps its merge and a branch; what it held goes to a test block after it. A value the test block
  works out is worked out again where it's read, as the unroller doesn't walk that block where it unrolls or counts
  the loop: one of no store or call, loading only what the shader can't write.
- **A continue block** of its own takes the branch back, with a counter's step `i + k` or `i - k` that only its
  header's Phi reads, as GLSL's `for` has it, so a block's int counts the loop as a hardware loop; a do/while's test
  moves there.
- **What a loop leaves with:** a merge's Phi of a header's Phi from the test shares that Phi's variable, stored at each
  break, as GLSL's one local; a value made before the loop is stored on each edge into the header; a value a
  do/while's pass works out, read after it, comes from the variable of the header's Phi taking it on the branch back.
- **Refused, named:** a do/while's header Phi read after the loop, whose variable holds the next pass's value; a value
  the test block works out kept past the loop; a value a do/while's pass works out read after it that no header Phi
  carries; a merge Phi of a header Phi that can't share its variable; a Phi on the edge from a test into the pass; and
  a switch, which spirv-opt's inliner makes of a helper's early return.

| | Corpus, 157 | Idioms, 21 |
|---|---|---|
| Compiled | 157 | 14 |
| A matrix not loaded from uniforms (step 5) | 0 | 2 |
| A constant array indexed by a loop's counter (step 5) | 0 | 2 |
| An int or bool constant (step 5) | 0 | 2 |
| An int vector (step 5) | 0 | 1 |

Of the five loop idioms, `i16` (a loop to an int in `$Global`) compiles, a hardware loop the block's int counts, its
need `("block_loop", 16, "uniform", 0, 0, 128, 0, 1)`; the other four reach step 5's causes: `i07` and `i15` their
static const arrays indexed by the counter, `i10` its bool constant, `i11` its varying `float3x3`. check_hlsl.py names
those two new causes and this step's refusals. The corpus has no loops: every one of its 157 results is step 4b's, 151
exact and the same 6 inexact.

**On the simulator:** `tests/test_hlsl_loops.py`, new, runs `xsc/testdata/hlsl/loops`, each shape beside its GLSL twin,
every lane alone and all in one group, the optimiser off and on, on inputs no operation rounds: every colour is the
twin's bit for bit and the loop's exactly, its loops take the twin's way, and each program is no longer than the twin.
ALU and control flow instructions, optimiser off, then on, HLSL and GLSL alike unless given:

| Shape | Loop | Off | On |
|---|---|---|---|
| `counted`: a constant count, an accumulator | unrolled | 19, 6 | 9, 4 |
| `while`: its counter stepped before a break on data | unrolled | 65, 12 | 48, 10 |
| `break`: a constant count left on data | unrolled | 90, 16 | 63, 12 |
| `exits`: a value set before it, a header Phi read twice, counts 0, 2 and 8 | block's int | 30, 10 | 30, 10 |
| `nested`: the inner loop left on data | unrolled | 175, 32 | 94, 18 |
| `continue`: a continue on data | unrolled | 42, 8 | 23, 6 |
| `dowhile`: what the pass works out read after it | 255 passes | 23, 8 | 23, 8 |
| `uniform`: an int in `$Global`, left on data, counts 0, 1, 3 and 8 | block's int | 18, 8 | 18, 8 |
| `data`: a value the test works out read in the pass and after | 255 passes | 27, 8 (GLSL 27, 10) | 27, 8 (27, 10) |

It runs xsdk_test's `hlsl/grid_loop_ps.hlsl` too, the cell's 80 columns in one group, 12 passes, against the test's
CPU reference, and three refusals, `refused_last_ps`, `refused_exit_ps` and `refused_kept_ps`. A break on a value
every pixel shares at a hardware loop's pass end still costs more than GLSL's: a probe of `uniform` with `if (i *
Step.w > Limit.y) break;` took 18 ALU instructions against the twin's 10, as its break block and the block branching
back hold stores the unroller's predicated LOOP_END shape (src/xsc_unroll.c, `ends_pass`) doesn't take, so it's gated.

Each piece broken on purpose failed it: Phis not stored at a break `while_ps.hlsl ... [2.03125, 4.0, 0.375, 1.0], the
GLSL [3.296875, 5.0, ...]`; the header not split `unexpected loop header terminator`; the counter's step left in the
pass `a loop counted by a block, stepping i other than by a constant past 0 in its continue block alone`; a value made
before the loop not stored before it `local %130 read before write`; a Phi's value not loaded again where it's read
`loop trip count exceeds the unroll cap`; the test block's values not worked out again and a do/while's not read from
its header's variable `a hardware loop's value read outside its pass`; merge Phis not sharing `a loop's header's Phi
left with apart from its own variable`; each refusal taken out, its shader compiled or refused for another cause.

**xost's tests:** every `tests/test_*.py` but `test_xcomb_parity.py` passes, `test_scalar_unit.py` with its corpus at
`/mnt/z/vulkan4xenon`.

**xsdk_test:** `tests/test_hlsl.c` draws `grid_loop_ps.hlsl` in a sixth cell, at row 645: a hardware loop an int in
`$Global` counts, 12 passes from its block loop need, each pixel adding a sixteenth of red and a thirty-second of green
a pass and leaving once its red passes TEXCOORD0's x, blue the pass it left at, so the columns leave at every pass and
the last 20 never. Each pixel is checked against the same loop on the CPU: `hlsl: a hardware loop an int in $Global
counts, each pixel leaving it once its red passes TEXCOORD0's x, draws what it carries and the pass it left at: %u of
%u pixels wrong`. `tests/breaks/hlsl/loop.patch` leaves a merge Phi sharing its header's variable unstored at a break:
on the simulator 60 of the 80 columns go wrong, each pixel that broke a step short, failing that check and
`xsc: hlsl/grid_loop_ps.hlsl compiles into the PC's container, literals and resources`.

**GLSL unchanged:** the 900 programs of step 2, compiled by f69e026 and by this step: the same containers, manifests
and resources byte for byte, and the 30 refused refused alike. `tests/xsc_shaders.h` regenerates as it is, plus the
grid's loop.
