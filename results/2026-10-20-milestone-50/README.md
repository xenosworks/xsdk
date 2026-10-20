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

## Step 5: the rest

`step5.txt`, `step5.json` (docs/scope/50-hlsl.md, step 5), each in shared code GLSL reaches too:
- **Constant arrays:** at a constant index, an unrolled loop's counter among them, the element itself, folded by the
  unroller from the constant or construct that made the local whole (src/xsc_unroll.c, `element`); at a run-time index,
  a hardware loop's counter among them, the array's elements in literal registers of their own, one each, a float's in
  every component, read at a0 as a block's array is (src/xsc_lower.c, `xsdk_xsc_literal_array`; src/xsc_lower_block.c,
  `xsdk_xsc_constant_element`). GLSL's constant arrays at a run-time index, refused before, take the same path.
- **Matrices not loaded from uniforms:** one built of columns is its columns' registers, read as columns; a leading
  block of one matrix's columns, as `(float3x3)M` takes, that matrix's first registers as they are, so i01's cast of a
  matrix glslang holds by rows is 3 `dp3`s (src/xsc_lower_matrix.c, `construct_matrix`). A matrix varying with a
  semantic takes an interpolator a column from the semantic's index (src/xsc_resolve.c, `add_varying`): TEXCOORD2's
  `float3x3` is TEXCOORD2 to 4, a row of the HLSL each; a pixel shader reads them as its columns, i11's tangent frame,
  and a vertex shader writes one made of columns. A matrix's rows are dotted as wide as the vector, so i09's `float4x3`
  bones, held by rows, take `dp4`s where they took `dp3`s and dropped the translation.
- **Bools:** an int or bool in HLSL's blocks is its register's float, as D3D9 sets HLSL's constants, i10's and i17's,
  HLSL alone by OpSource, as a Vulkan block holds an int's bits; a constant bool vector's lanes are 1.0 and 0.0, where
  they read 0.0; and a shuffle reading its second vector, which spirv-opt folds a select on a constant bool vector to,
  is a masked copy a lane, the second vector's components counted past the first's, not past 4.
- **Int vectors:** an int vector's component, an int vertex input, which its fetch makes floats as UBYTE4's are, a
  component of one through an access chain, and a0 loaded straight from an int vector's lane, as skinning's
  BLENDINDICES index bones; i09's `D3DCOLORtoUBYTE4` is float work and a `trunc`.
- **A lone PSIZE:** an out parameter, which glslang makes a PointSize of its own, is oPts.x, step 4's export.
- **And what the idioms needed besides:** a unary minus of a value (FNegate), a copy with the negate modifier, i01's;
  and `exp`, `log`, `exp2`, `log2`, `sqrt`, `inversesqrt`, `sin` and `cos` of a vector a component at a time,
  i15's, where the scalar unit read one component, w or x, for every lane.

| | Corpus, 157 | Idioms, 21 |
|---|---|---|
| Compiled | 157 | 21 |

The corpus's results are step 3's, shader by shader: 151 exact, the same 6 inexact.

**On the simulator:** `tests/test_hlsl_rest.py`, new, runs `xsc/testdata/hlsl/rest`, each shader beside its GLSL twin
where one is written, the optimiser off and on, every export exact and the twin's bit for bit: `array_unrolled_ps`
(float and float2 arrays at unrolled counters), `array_indexed_ps` (a float4 and a float array at a block-counted
hardware loop's counter, counts 0, 3 and 6, and at an index from TEXCOORD0), `matrix_cast_vs`, `matrix_tbn_ps` and
`matrix_tbn_vs` linked into it, `bool_ps` (a bool in `$Global` at 0 and 1, float4 and float3 two-source shuffles, a
bool4 of constants and a comparison; the twin's a constant bvec4 select), `vector_exp_ps`, `psize_vs`, `skin_vs`
(i09's), `grid_skin_vs` (xsdk_test's, int4 BLENDINDICES) and `negate_vs` (a scalar int input and an int4's
components, one negated), and two refusals, `refused_array_ps` and `refused_rows_vs`. Each of 24 changes broken on
purpose failed it: the array fold `array_unrolled_ps ... [-4.8125, 12.375, ...], not [0.078125, -5.28125, ...]`; the
run-time chain refused; the literal registers one up `array_indexed_ps ... [-0.5, 2.5, 1.25, 1.0], not [1.5, 1.25,
1.125, 2.0]`, a float's not in every component `[1.5, 0.25, 0.125, 1.0]`; the constant array not found `a run-time
index into a local array other than a constant one`; a cast's majorness flipped `refused_rows_vs.hlsl compiled`; a
construct's columns read as rows `matrix_tbn_ps ... the GLSL twin [0.375, -1.5, 0.9375, 1.0]`; a matrix input's
columns one register `[0.625, 0.3125, -1.25, 1.0]`, one interpolator `[0.375, 0.1875, -0.75, 1.0]`, from its last
`[0.09375, -0.375, 1.5, 1.0]`; an output's column always 0 `matrix_tbn_vs linked ... [0.625, 0.3125, -1.25, 1.0]`;
the dot width the rows' `skin_vs ... [1.1796875, 1.109375, 1.0390625, 1.0], not [0.921875, 0.6015625, 0.40625, 1.0]`;
FNegate not negating `matrix_cast_vs d ... [0.5, -1.0, 0.25, -1.109375]`, a scalar's `negate_vs ... [0.75, 3.0, ...]`,
a scalar not spread `[0.75, -7.0, ...]`; bool lanes 0 `bool_ps ... the GLSL twin [23.75, ...]`; the second vector's
components past 4 `bool_ps ... [6.0, 5.75, 5.5, 3.25], not [7.25, 7.0, 6.75, 4.5]`; HLSL's block ints and int inputs
`integer Load must be a foldable local`; an int extract's lane 0 `negate_vs ... [0.75, -3.0, 5.0, 8.0]`; a0 from an
extract's x `skin_vs ... [1.0625, 0.8125, 0.6875, 1.0]`; an input chain's lane 0 `grid_skin_vs's position ... the GLSL
twin [1.0625, 0.8125, 0.5, 1.0]`; PSIZE unread `gl_PointSize ... outside gl_PerVertex`; each lane's transcendental
from x `vector_exp_ps ... [2.3987..., 2.3987..., 2.1798..., 2.1798...]`.

**xost's tests:** every `tests/test_*.py` but `test_xcomb_parity.py` passes, `test_scalar_unit.py` with its corpus at
`/mnt/z/vulkan4xenon`; `test_hlsl_link.py` checks the export slots of 66 HLSL vertex shaders now, `rest/`'s among them.

**What stays refused** (for docs/xsc.md's HLSL section, step 8), each by name: a local array of other values at a
run-time index, or stored by element; a matrix output held by rows, as a cast of a block's matrix, where an
interpolator takes a column; a vertex shader's matrix input; a column of a matrix value other than a cast's; an int or
bool load other than a local's, an input's, gl_InstanceIndex or HLSL's in a block, a GLSL block's among them, whose
int bits xost doesn't read; and int operations past add, subtract, multiply, divide and remainder, the bitwise and
shift ones Xenos lacks. Not refused and wrong, as before this step: `pow` of an exponent whose components differ takes
its x in every lane. Step 6's: i10's `bool : register(b0)` and `int : register(i0)` sit in `$Global` at c0.x and c0.y,
over `LightColor[0]`, and its loop's count is read from the block's bits as a Vulkan int.

**xsdk_test:** `tests/test_hlsl.c` draws `grid_skin_vs.hlsl` with the link's pixel shader in a seventh cell, left of the
loop's at column 860, row 645: each corner of a quad skinned by four of 8 `float4x3` bones in vertex constants c0 to
c23 that its int4 BLENDINDICES pick, UBYTE4s, weighted a half, a quarter and the rest, each corner's bones a pair whose
changes cancel at their weights, so the quad lands on the cell with red from 0.25 to 0.75 and a bone read at a wrong
index moves or reshades it: `hlsl: a quad skinned by float4x3 bones in vertex constants that its corners' int4
BLENDINDICES, UBYTE4s, pick lands and shades as the CPU's skinning of its corners: %u of %u pixels wrong`.
`tests/breaks/hlsl/skin.patch` loaded a0 from an int vector's x for each component: on the simulator every corner takes
its first bone four times and the quad collapses to no width at column 1019.5, failing that check and `xsc:
hlsl/grid_skin_vs.hlsl compiles into the PC's container, literals and resources`. The console drew it right, so the
break is now another ("Step 5's skin break drew right on the console", below). The link break leaves this pair as
it is: its stages pack TEXCOORD0 and COLOR0 alike.

**GLSL unchanged:** the 900 programs of step 2, compiled by 4e37768 and by this step: the same containers, manifests
and resources byte for byte, and the 30 refused refused alike. `tests/xsc_shaders.h` regenerates as it is, plus
`grid_skin_vs.hlsl`.

## Step 6: binding as D3D9 binds

`step6.txt`, `step6.json` (docs/scope/50-hlsl.md, step 6); the JSON now holds each compiled shader's constant table.
glslang keeps an HLSL sampler's `register(sN)` as binding N, a sampler without one binding 0, and drops `b#` and `i#`:
`i` is an unrecognized register type, and `b` sets a binding a `$Global` member doesn't keep
(glslang:glslang/HLSL/hlslParseHelper.cpp:6396-6431); it packs bools and ints into `$Global` beside the floats. So,
HLSL alone, by OpSource (xenos-toolchain/docs/xsc.md, "Interface conventions"):
- **Samplers** take their binding as their fetch constant, as `SetTexture(N)` binds them (src/xsc_resolve.c,
  `hlsl_slots`); one sharing a binding, a sampler without a register beside another or beside one at s0, the lowest no
  binding names. glslang gives `s0` and no register alike, so a sampler at s0 declared after one without a register
  takes another constant; the table says which. GLSL still packs from 0 by set and binding.
- **A bool or int in a register another member holds** takes a float register of its own past the blocks
  (`move_shared`); one alone in its register stays there, as `register(cN)` puts it. Read as a value it is that
  register's float (src/xsc_lower_block.c, `xsdk_xsc_integral_place`).
- **A bool a pixel shader's if reads,** where the if's arms go in execs (a shader the unroller runs on, its arms
  working something out), is a boolean constant from 128, libxenon's pixel half, the arms in `cexec b128` and
  `cexec !b128` execs, opcode 13 as milestone 11's console probe ran `cexec b128` (docs/HARDWARE.md, "Shader constants:
  partial writes hang the GPU, libxenon's whole blocks don't"); `!b` is new to the console. An arm there that uses a0
  is refused, and the optimiser leaves a program with such execs alone, as their ops aren't predicated. Elsewhere, a
  select as i10's or a vertex shader's if, the bool is a value.
- **An int a loop counts** is the loop constant that loop reads, as step 3's block loops had it, set to
  `xsdk_gpu_loop_constant(n, 0, 1)`; a loop from other than 0 by 1 is refused, as a title setting the int by name can't
  count its passes.
- **The constant table** names each sampler and member as declared, with its register set, index and count, and its
  type: class, bool, int or float, HLSL's rows and columns, array elements, a matrix `column_major` by columns
  (src/xsc_lower_stage.c, `xsdk_xsc_uniform_constants`); a bool read as a boolean constant is in the bool set, an int
  a loop counts in the int4 set, and either read as a value in the float4 set. GLSL's table is as it was.
  `xsdk_shader_constant` (include/xsdk/shader.h) finds one by name.

| | Corpus, 157 | Idioms, 21 |
|---|---|---|
| Compiled | 157 | 21 |

Every result is step 5's, shader by shader: 151 exact and the same 6 inexact. The tables now say:
- **m4:** A at s3 is fetch constant 3, B at s1 is 1, C without a register 0, where step 5 packed C, B and A at 0, 1
  and 2; K at c5.
- **i10:** UseTexture, at b0, which glslang put at c0.x over `LightColor[0]`, is c8, a float register of its own past
  the block's c0 to c7, read as a value as its `?:` is a select; LightCount, at i0, which glslang put at c0.y, is loop
  constant 16 in the int4 set, its loop's, and no float register; LightColor and LightDir are c0 and c4, 4 registers
  and 4 elements each.
- **i16 and i17:** LightCount is loop constant 16; UseTexture, alone at its `register(c0)`, stays c0.

Of the HLSL testdata, only i10's and m4's microcode change; every other container gains its table's names and types.

**On the simulator:** `tests/test_hlsl_binding.py`, new, runs `xsc/testdata/hlsl/binding`, the optimiser off and on:
`slots_ps` (samplers at s1 and s4 sample 1 and 4, two without a register 0 and 2) and its GLSL twin `slots.frag`
(packed 1, 3, 0 and 2); `integers_ps` (Flip at b3 in an if, boolean 128; Count at i2, loop constant 16; Tint at b1 and
Scale at i0, values, c5 and c7, glslang having packed all four into c2, which holds 99 there; each Flip and Tint, Count
0 and 2); `unrolled_ps` (a bool's if past an unrolled loop, which the optimiser would take); `table_ps` (every entry);
xsdk_test's `grid_binding_ps`, its cell drawn and checked as the console's; and `refused_step_ps`. Each of 17 changes
broken on purpose failed it: samplers packed from 0 `slots_ps ... samples fetch constants [2.0, 3.0, 0.0, 1.0], not
[1.0, 4.0, 0.0, 2.0]`; a shared binding's sampler at 0 `[1.0, 4.0, 0.0, 0.0]`; shared ints not moved `Tint ... and
Scale ..., not floats of their own`; a moved member read where glslang put it `[198.0, -173.25, 272.25, 99.0]`; no
boolean constants `Flip in the bool set: []`; a bool's arms swapped `[4.5, 12.0, -7.5, 3.0], not [1.5, -3.75, 6.0,
2.25]`; no loop constant entry `Count in the int4 set: []`; other loop shapes not refused `refused_step_ps compiled`;
matrices' class flipped, a vector's columns 4, elements written 1, each `table_ps ...`; a bool unread as a float
listed `Flip or Count read as a float`; HLSL samplers named tex<slot> `A in the sampler set: []`; a cexec's boolean a
bit low, its condition dropped, its ops predicated, each `integers_ps ...`; the optimiser let in `unrolled_ps,
optimiser on, Flip True: [1.0, -2.25, 5.0, 1.0], not [2.0, 10.0, -4.5, 2.0]`. `xdis/simulate.py` runs execs on
boolean constants, `bools` the true ones.

**xost's tests:** every `tests/test_*.py` but `test_xcomb_parity.py` passes, `test_scalar_unit.py` with its corpus at
`/mnt/z/vulkan4xenon`. WSL's binfmt entry for Windows `.exe` files was gone in this session, so the tests that run
XenosRecomp, and check_hlsl.py's glslang, launched them through `/init` by hand from a scratch `sitecustomize.py`.

**xsdk_test:** `tests/test_hlsl.c` draws `grid_binding_ps.hlsl` in an eighth cell, left of the gaps' at column 860,
row 600, in two quads: Low at s3 and High at s7 sample 4-texel point-sampled textures bound with
`xsdk_gpu_set_texture(3)` and `(7)`, other textures at 0 and 1; Step, Bright and Count are found by name with
`xsdk_shader_constant` and set there, the left half Bright with Count 2, blue 191, the right half not with Count 5,
blue 159. Checks: `hlsl: samplers at s3 and s7 sample the textures bound at slots 3 and 7, an if reads a bool at b2
and a loop counts an int at i1, each set by name through the container's table, as D3D9 binds them: %u of %u pixels
wrong`, and `hlsl: grid_binding_ps.hlsl's table names Low at fetch constant %d and High at %d, as their s3 and s7, Step
at float constant %d, Bright at boolean constant %d and Count at loop constant %d`, which wants 3 and 7 and every name
found. `tests/breaks/hlsl/binding.patch` packs HLSL's samplers from 0 again: on the simulator its container draws all
80 columns red 8 and green 8, the textures at 0 and 1, 28800 of the check's 28800 pixels wrong over its 9 frames, and
its table puts Low and High at 0 and 1; an empty cell, the clear colour, is 28800 wrong too, and the PC's container 0.
It must fail both checks and `xsc: hlsl/grid_binding_ps.hlsl compiles into the PC's container, literals and
resources` on the console.

**GLSL unchanged:** every GLSL program of step 2, 451 now with xost's newer testdata, the optimiser off and on,
compiled by 5dd03e9 and by this step: 902 containers, manifests and resources the same byte for byte, the 30 refused
refused alike. `tests/xsc_shaders.h` regenerates with `grid_binding_ps.hlsl`; of the others only HLSL tables change,
grid_loop_ps's Count an int at loop constant 16, grid_skin_vs's bones 8 float4x3s.

## On the console

docs/HARDWARE.md, "HLSL on the console, milestone 50", records what these settled. Each run built clean in full from
M50 with vulkan4xenon against it (local/m50/build.sh), through the console agent on 2026-10-20:
- **Step 4** (`step4_*.log`, at 11a3e72): xsdk_test 488 of 490, the controller's two failing with the pad idle; the
  grid's link, VPOS, VFACE and DEPTH checks pass, VFACE +1 on a clockwise front face. Each break failed the checks it
  names and its shader's compile check. vulkan4xenon_test 492 of 492 twice.
- **Step 4 linked at bind** (`step4b_*.log`): xsdk_test 512 of 514 with the gaps pair's check and the 2-interpolator
  check passing; the link break failed the three link checks, the others theirs. vulkan4xenon_test 492 of 492 twice,
  vkbench 17 of 17.
- **Step 3** (`step3_*.log`, at 245091a): xsdk_test 514 of 516, the loop's cell passing. The loop break failed the loop
  check and its compile check; the link break the three link checks and the loop's, whose cell links its stages too.
  vulkan4xenon_test 492 of 492 twice, vkbench 17 of 17.

## Step 5's skin break drew right on the console

At cdcbb2f, xsdk_test's skinning check passed, 0 of 28800 pixels wrong (`step5_xsdk_test.log`), and so did the skin
break's build, skin.patch then loading a0 from an int vector's x for each component, which failed only `xsc:
hlsl/grid_skin_vs.hlsl compiles into the PC's container, literals and resources` (`step5_xsdk_test_hlsl_skin.log`). On
`xdis/simulate.py` that break's container collapses the quad to no width at column 1019.5.

**The containers**, grid_skin_vs.hlsl at xsdk's split with the optimiser on: the PC's, 916 bytes, as
`tests/xsc_shaders.h` holds it, and the break's, 892. r2 is BLENDINDICES' fetch and c24 the literal 3.0, a bone's
registers. Their four a0 loads, by instruction slot:
- **Index x,** both: 9 `mul r4.x___, r2, c24`, 10 `maxas r0._, r4.xx`.
- **Index y:** the PC's 14 `mul r4.___w, r2.xzwy, c24.xzwy`, 15 `maxas r0._, r4.ww`; the break's 13 `maxs r2.___w,
  r4.xx` beside `dp4 r2.__z_, r0, c[2+a0]`, 14 `maxas r0._, r2.ww`.
- **Index z:** the PC's 19 `mulsc r4.___w, c24.x, r2.z` beside a `mul`, 21 `maxas r0._, r4.ww`; the break's 18
  `maxs r2.___w, r4.xx` beside a `mul`, 20 `maxas r0._, r2.ww`.
- **Index w:** the PC's 26 `mul r4.___w, r2, c24`, 27 `maxas r0._, r4.ww`; the break's 23 `maxs r2.___w, r4.xx`
  beside `dp4 r2.__z_, r0, c[2+a0]`, 25 `maxas r0._, r2.ww`.

Each load is followed by the same three `dp4`s at `c[0+a0]` to `c[2+a0]`; the rest is the weighted sum, reordered.

**How they're read.** A scalar op takes src3's a from the swizzle's bits 6-7 plus 3 and b from bits 0-1
(xenia:src/xenia/gpu/ucode.h:952-953, shader_translator.cc:1242-1248; XenosRecomp:shader_recompiler.cpp:538-546),
and `maxas` sets a0 to a rounded (ucode.h:1138-1143, shader_interpreter.cc:808-815), as `xdis/simulate.py` does
(simulate.py:144, 199-201). Every load in both containers is a broadcast, `.xx` (0x6C) or `.ww` (0x1B), whose four
fields all name one lane, so a, b and any field the console might take read the same component. The lane question of
docs/HARDWARE.md, "The scalar unit's constant forms, milestone 48 step 5", concerns `mulsc`, `addsc` and `subsc` with
both components one; the PC's one such form, `mulsc c24.x, r2.z`, has two, which that probe found right. Broadcast
loads of other lanes already draw right on the console: `vk_indexed_reads.vert`'s `maxas r1.yy`, `maxasf r0.ww` and
`.zz`, `vk_dynamic_interpolator.vert`'s `maxasf r0.zz`, `vk_dynamic_texcoord.frag`'s `.yy`.

**Why the lane can't explain the pass.** The break's container works out 3 times index x alone: r2's y, z and w are
overwritten by bone rows and a copy of r4.x at 11 to 13, before any later load, and the copies at 13, 18 and 23 put
r4.x in r2.w. A later load can find 3 times index x, index w unscaled, bone rows, partial sums or r4's lanes from
before the shader, so no choice of lane, no late write and no co-issue rule gives a corner its other bones; the top
right corner, bones 4 to 7, needs a0 15, 18 and 21. A bone read four times moves its corner by that bone's change, so
the quad can't land right. On the PC the
break compiles to those 892 bytes on x86-64, as a 32-bit big-endian powerpc-linux build under qemu-ppc, with
`-ftrivial-auto-var-init=pattern`, with `MALLOC_PERTURB_` and under ASan and UBSan; and the break build's console
object of xsc_lower_block.c has the call reading the lane gone. So either the console didn't draw this container in the
cell, or the skinning check can't see on the console which bone is read. These runs can't tell which.

**The PC's container** reads lane k times 3 for bone k on the simulator, through vector ops moving the lane to w and
broadcast loads, every form the console has drawn right before. Its pass on the console shows the quad lands; with the
break's pass beside it, it doesn't show the bones were told apart.

**The probe**, `tests/probe_skin.c` (`make skin_probe`, `build/probe_skin.xex`, its log `probe_skin.log`), draws the
cell as tests/test_hlsl.c does, 3 frames a case, the last 2 checked against the CPU's skinning, and logs the box of
every pixel not the clear colour. Its xost is the old break's: `tests/probe_skin.patch` on a copy of xsc_lower_block.c,
linked ahead of libxsdk.a, and built into the PC's compiler for the lanex container. Every case runs, or the one
`game:\skin_start.txt` names. What the simulator draws, from the CPU's quad over columns 859.5 to 939.5, rows 644.5 to
684.5:
- `pc`, the PC's container: right, 0 of 6400 wrong, over columns 860 to 939, rows 645 to 684.
- `lanex`, the old break's PC container: nothing drawn, 6400 wrong.
- `title`, the title's own compile by the old break: its bytes logged against lanex's, then drawn; as lanex.
- `swapped`, the PC's container with bones 0 and 1 swapped in the constants: the left corners at column 739.5, so over
  columns 740 to 939, most of the cell wrong.

What it settles: lanex and title drawing nothing and swapped wrong means the console reads these loads as the simulator
does, and the break run's pass came from something else, so that build runs again; lanex right and swapped wrong
means the console reads one of these loads otherwise, to be split up instruction by instruction next, as
probe_relative_constants.c's cases are; swapped right as well means bones 0 and 1 aren't told apart on the console and
the skinning check is blind to the index, which the test, not xost, must fix. A title compile that differs from lanex
logs its first differing dword.

**The skin break now** (tests/breaks/hlsl/skin.patch) loads a0 from an int vector's lane as a float index, floored and
not times the bone's 3 registers: `maxasf r0._, r2.xx` to `r2.ww` straight from the fetch. Only index 0, 3 and 6 are
whole bones, and every load misreads some corner whichever lane it reads (any lane but z gives the top right corner 4, 5
or 7, z gives the top left 2), so the quad goes wrong even if every bone held the same rows. On the simulator the
corners land at columns 711, 501, 661 and 529, rows 522, 173, 518 and 225, leaving the cell empty;
`tests/test_hlsl_rest.py` fails `skin_vs, optimiser off: [-0.2109375, 0.4609375, 0.171875, 1.0], not [0.921875,
0.6015625, 0.40625, 1.0]`; of xsdk_test's 56 checked shaders only `grid_skin_vs.hlsl` compiles otherwise. It must
fail `hlsl: a quad skinned by float4x3 bones ...` and `xsc: hlsl/grid_skin_vs.hlsl compiles into the PC's container
...` on the console.

**Proposed for docs/HARDWARE.md** once the probe has run, under "HLSL on the console, milestone 50":
- **The skin break that drew right.** xsdk_test built with a0 loaded from each int4 index's x passed its skinning
  check, 0 of 28800 pixels, with a container that on Xenia's model (xenia:src/xenia/gpu/ucode.h:1138-1143) reads the
  first index's bone four times and collapses the quad; it works out no other index, so no reading of its loads' lanes
  explains it. `tests/probe_skin.c`'s result goes here.

## The skin check was blind, and is fixed

The probe answered (`step5_probe_skin_blind.log`): the break's container, compiled on the PC and on the console alike,
drew nothing, as on the simulator, and still counted 0 of its cell's pixels wrong; the swapped bones did too. The
check couldn't fail. `CLEAR_ARGB = 0xFF202020` sat in the enum with `CELL_HEIGHT`, and a value past INT_MAX gives an
enum's constants an unsigned type in GCC, so `-CELL_HEIGHT / (HEIGHT / 2.0f)` came out +11930465 where -0.111 was
meant: the palette threw the quad's bottom edge off the screen's top, where the console and the simulator both drew it
(rows 0 to 644), and the reference's box was empty, so the check expected the clear everywhere it looked. 62b7e34
moves the colours out of the enum and checks the gaps, loop and skin cells' middle columns, which a DEPTH-margin
`continue` had skipped; cc9f057 puts the quad 8 pixels inside its cell and requires its shade inside its box and the
clear around it. docs/PITFALLS.md, "Colours in an enum", warns of it.

On the console, at cc9f057 rebased on main 17c6e91 (`step5_*.log`):
- xsdk_test 522 of 524, the controller's two failing with the pad idle; the skin check passes, "lands where the
  palette puts it, the clear around it", 0 of 28800 wrong.
- The skin break (maxasf, the index unscaled) fails the skin check and its compile check.
- `step5_probe_skin.log`: the PC's container right, drawn over columns 868 to 931, rows 653 to 676; the old break's
  container, PC's and console's alike, 3072 of 6400 wrong, drawn at columns 1012 to 1027; bones 0 and 1 swapped, 3408
  wrong. The console agrees with the simulator.
- vulkan4xenon_test 515 of 515 twice, vkbench 17 of 17.
- The loop, link, VPOS, VFACE and DEPTH breaks at cdcbb2f (`step5_xsdk_test_hlsl_*.log`) each failed what they name.
