# xsc — GLSL → Xenos shader compiler

Pipeline: **GLSL → glslang → SPIR-V → xsc → Xenos container**, and shader model 3
HLSL the same way (HLSL, below). glslang
(Khronos reference compiler) does parsing/semantics/optimization; xsc lowers
the SPIR-V to microcode through the hardware-proven Program builder. xsc is C,
xsdk's `src/xsc_*.c`: titles call `xsdk_xsc_compile` (`include/xsdk/xsc.h`), and
on the PC `xsc/lower.py` calls the same code built as `build/host/libxsc.so`.
Until 2026-10-14 it was Python; the C port gives the same bytes. The
Mesa/NIR optimizer is a someday option; the runtime combiner case is already
served by `xcomb`.

## Use

```
python3 -m xsc.lower shader.vert.spv -o shader.bin     # SPIR-V -> container, in WSL
```
Library: `lower(module) -> bytes`, `lower_ex(module) -> (bytes, manifest)`.
Get the SPIR-V with: `glslang -G --target-env opengl shader.frag -o out.spv`,
or Vulkan's with `glslang -V shader.frag -o out.spv`: vertex inputs, varyings
and outputs lower the same either way (`tests/test_vulkan_spirv.py`).
Vulkan's uniform blocks, push constants and samplers land as "Interface
conventions" says; `resolve_interface(module).blocks`, `.samplers` and
`.comparisons` give where.

Instruction templates (the field skeletons the lowering fills in) come from
Xenia's definitions, `xsc/templates.py`. Before xost joined xsdk,
`tests/test_templates.py` checked them against the instructions they first
came from; that test and its test vectors stay in xost's archived repo (github.com/xenosworks/xenos-toolchain, last commit 4a785c6).

## Supported (v1, branch-free)

| GLSL / SPIR-V | Xenos lowering |
|---|---|
| vertex `mvp * pos` (MatrixTimesVector) | 4× `dp4` → oPos |
| vertex chained `mat4 * (mat4 * pos)` | first mat4 x vec4 into a temp, second mat4 x vec4 -> oPos |
| vertex `vec4 * mat4` (VectorTimesMatrix) | row-vector `mul`+`mad` |
| vertex `(mat4 * mat4) * vec4` | product rows via `vec*mat`, then 4× `dp4` |
| vertex `+` `-` `*`, `vec * scalar`, `dot()`, swizzled temps | same ALU lowering subset as fragment, feeding position or interpolator exports |
| passthrough varyings | interpolator exports (max o*N*) |
| `texture()` (ImageSampleImplicitLod) | `tfetch2D` |
| samplerCube `texture()` | cube swizzles + rcp/bias + `tfetchCube .yxw`; each face and its s and t as Vulkan picks them, on the simulator with the cube op as Xenia runs it (`tests/test_cube_and_3d.py`) |
| sampler3D `texture()` | `tfetch3D` at the coordinate's `.xyz` |
| sampler2DShadow `texture()` (OpImageSampleDref) | `tfetch2D`, then the reference clamped to 0-1 (`max_sat`), `sgt`/`seq`/`sgt` for the reference below, equal to and above the depth, and `dp3` with the comparison's register (see Interface conventions; `tests/test_shadow_compare.py`); samplerCubeShadow refused, and textureProj's comparison not lowered |
| `*` `+` `-` (FMul/FAdd/FSub) | mul / add / add-neg |
| `/` (FDiv) | constant divisor: `a · (1/b)`, the float nearest the reciprocal, worked out by the compiler (a lane of 0, infinity, NaN or a denormal keeps `rcp`); otherwise `a · rcp(b)`, scalar-pipe `rcp` per component (scalar divisor: once + broadcast) (`tests/test_division.py`) |
| `mod()` (OpFMod) | `a − b·floor(a/b)` |
| `vec * float` (VectorTimesScalar) | mul, scalar `.xxxx` broadcast |
| scalar int -> float | folded literals or float-backed `trunc` + add/mul |
| int vertex inputs, an int vector's components, HLSL's ints and bools in a block | the fetch's floats, an int in x, a component spread over a register; a block's int or bool its register's float, as D3D9 sets HLSL's constants, HLSL alone, as a Vulkan block holds an int's bits (`tests/test_hlsl_rest.py`) |
| unary `-v` (FNegate) | a copy with the negate modifier, a scalar spread |
| `dot()` / `cross()` | `dp2add` with a zero third source (vec2), `dp3`, `dp4`, broadcast / yzx·zxy mad |
| `mix` / `max` / `min` / `clamp(x,0,1)` (GLSL.std.450) | sub+mad / max / min / `_sat` modifier (free) |
| `step` / `smoothstep` | `kSge` / `t·t·(3−2t)`, `t=sat((x−e0)/(e1−e0))` |
| `normalize` / `length` / `distance` | dp3·rsq·mul / dp+sqrt / sub+dp+sqrt (scalar pipe) |
| `sqrt` `inversesqrt` `exp2` `log2` `exp` `log` | scalar-pipe `kSqrt`/`kRsq`/`kExp`/`kLog` (+const for natural), a vector's a component at a time, each result masked into its lane |
| `pow(x,y)` | per-component `exp2(y·log2 x)` (scalar pipe) |
| `reflect(i,n)` | `i - 2·dot(n,i)·n` (dp + mad) |
| `sin` / `cos` / `tan` | scalar `kSin`/`kCos`, a vector's a component at a time; `tan = sin·rcp(cos)` — **range-limited ~[-π,π]** (see below) |
| `asin` / `acos` / `atan` | polynomial approx (A-S sqrt form / π/2−asin / odd poly); ~7e-5 over [-1,1], atan range [-1,1] (see below) |
| `.rgb` `.xy` (identity) / `.bgra` (reorder) / two-source `vec4(a.xy,b.zw)` | same regs / swizzled mov / per-component masked movs; an OpVectorShuffle reading its second vector, as spirv-opt folds a select on a constant bool vector, a masked mov a lane |
| `v.x` (CompositeExtract), `vec4(a,b,c,d)` (CompositeConstruct) | masked exports |
| `floor` / `ceil` / `trunc` / `round` / `fract` | `kFloor` / `-floor(-x)` / `kTrunc` / `floor(x+.5)` / `kFrc` |
| `abs()` / `sign()` | `max(x,-x)` / `sgt(x,-x)−sgt(-x,x)` |
| `radians` / `degrees` | mul by pooled `π/180` / `180/π` |
| `clamp(x, lo, hi)` arbitrary | `min(max(x,lo),hi)`, lo/hi pooled literals |
| **`if/else`**, incl. **nested**, assigning the output | branch-free chained `cndgt`/`cndge`/`cndeq` selects |
| **`if/else` assigning a Function-local** later read by the output | branch-free `cnd*` select into a temp registered as that local's merged value (pixel shaders) |
| `if/else`, or a `?:` glslang branches for, in a **vertex** shader, nested, `&&`/`\|\|` (a Phi) | the unroller runs both arms and merges each local and output they leave apart with a select: a `cnd*` on the comparison branched on, on a condition every vertex shares too, as execs on the predicate hung the GPU (see Control flow; `tests/test_vertex_branches.py`) |
| **`discard` / `OpKill` inside `if(cond)`** | `kill_gt`/`kill_ge`/`kill_eq`/`kill_ne` ALU side effect; confirmed on the console, a `discard` on `!gl_FrontFacing` (vulkan4xenon milestone 22) |
| **`for`/`while`** with a compile-time trip count, incl. **nested** | unrolled to branch-free SSA; induction variable constant-folded (see below) |
| uniform-bound loop `sum += const` | real `LOOP_START/END` (G28 pending) |
| `for (int i = s; i < b.n; i += k)`, `n` an int in a push constant or uniform block, `s` and `k` constants, in a vertex or pixel shader | a hardware loop of ceil((n - s) / k) passes, which the runtime works out from the block, as Shader Model 3's `loop` counts them: what the loop carries in registers, `i` a float counter, an if/else in the pass, a break ending it; a `break` or `continue` on data anywhere in the pass gated as in an unrolled loop, LOOP_END breaking once no pixel of the group is left (see "Loops a block counts"; `tests/test_hardware_loops.py`, `tests/test_hardware_loop_breaks.py`) |
| a loop whose count is data: a `for` to `int()` of a float or an interpolant, `while` and `do`/`while` on data | a hardware loop of 255 passes, the most a loop constant counts, left on its condition, a gated break at the top of the pass or, for a `do`/`while`, at its end; the manifest's `("fixed_loop", id, 255)` (see "Loops a block counts"; `tests/test_data_loops.py`) |
| hardware loops nested 4 deep, and in an if/else on data or an unrolled loop's pass | each level its own loop constant, registers and gating (see "Loops a block counts"; `tests/test_nested_loops.py`) |
| a sample or derivative of what a hardware loop's pass works out | lowered in the pass, after what it reads (see "Loops a block counts"; `tests/test_loop_samples.py`) |
| `vec4 t = ...;` (function-local) | store-forwarding |
| uniforms (block + bare), samplers | constant regs / fetch consts |
| Vulkan uniform / push constant block members at any std140 offset | the member's register, swizzled from its first component |
| block matrix (column-major, std140) `M * v` / `v * M` / `A * B` | `mul`+`mad` over columns / 4x `dp4` with columns / columns of A times each column of B |
| block array at a run-time index, `rows[int(f) + k]`, `m[i]` | `movasf` (`int(f)`) or `movas` sets a0, then `c[a0 + base + k]` is copied into a temp; reads at one index share the `movas` (`tests/test_dynamic_index.py`) |
| constant array, `const float w[5]`, HLSL's `static const` | at an unrolled loop's counter or another constant index, the element itself; at a run-time index, a hardware loop's counter among them, its elements in literal registers of their own, one each, a float's in every component, read at a0 as a block's array is (`tests/test_hlsl_rest.py`); a local array of other values at a run-time index, or stored by element, refused |
| fragment `gl_FragCoord.xy`, `gl_FrontFacing` | the pixel parameters in the register past the interpolators, `\|r.xy\| + c255`, the constant xsdk keeps at 0.5 and a tiled pass's band's top row plus 0.5 (see Interface conventions), `.zw` refused; facing from x's sign (see Facing) |
| fragment `gl_PointCoord` | the same parameters' `\|r.zw\|`, a point sprite's coordinates (`tests/test_point_sprite.py`) |
| fragment HLSL `VPOS` and `VFACE`, by their semantics | the same parameters: VPOS `\|r.xy\| + c255.zw`, which xsdk keeps at 0 and a tiled pass's band's top row, the pixel's whole column and row, its z and w 0; VFACE a `cndge` on x's sign, +1 on a front face and -1 on a back one (see Interface conventions and Facing; `tests/test_hlsl_semantics.py`) |
| vertex `gl_PointSize` | `oPts.x___`, the point's width in pixels, and the record's vs_export_mode 2 (`tests/test_point_sprite.py`) |
| fragment `gl_FragDepth`, stored once outside any `if/else` | `oDepth.x___` after the colour, the record's ps_export_mode 3 (`tests/test_frag_depth.py`) |
| fragment `gl_FragDepth` stored in both arms of an `if/else`, or before one and again in an arm | merged by a select as a vertex shader's outputs are, exported once; stored on one path alone, refused (see Control flow) |
| a store into one component of a local or an output, `v.x = …` | an insert into the vector it held (`tests/test_local_stores.py`) |
| vertex `gl_InstanceIndex`, firstInstance included | t.y, the instance an instanced shader's prologue works out from the instance group, held in every component of a register (see Interface conventions); `float()` of it, integer arithmetic on it and a block array's index at it lower as integers do (`tests/test_instancing.py`). `gl_VertexIndex` is refused |
| fragment outputs at locations 0 to 3, each stored once outside any `if/else` when there are several | `oC0` to `oC3`, the four colour targets, a float in every component; the record's outputs mask names them (`tests/test_color_outputs.py`) |
| vertex `gl_ClipDistance[n] = dot(gl_Position, plane)`, n 0 to 5, the plane a whole vec4 of a uniform or push constant block | no instruction: user clip plane n, the manifest's `("clip", n, register)`; any other form, and `gl_CullDistance`, refused (`tests/test_clip_distance.py`) |
| float / vec literals in FS/VS | constant pool; 0/1 folds (see below) |
| ternary `cond ? a : b` (OpSelect) | `f + (t−f)·cond` (sub + mad) |
| vector relational `lessThan`/`greaterThan`/`equal`/`notEqual`/… → bvec | per-component `sgt`/`sge`/`seq`/`sne` (1.0/0.0) |
| `any` / `all` / `not` (bvec) | horizontal `max` / horizontal `min` / `1−v` |
| `&&` / `\|\|` | `min` / `max` of 0/1 bools |
| `refract(I,N,eta)` | Snell vector + total-internal-reflection (`k<0`) mask |
| runtime int `/` `%` (SDiv/SMod/U*) | `trunc(a·rcp(b)+ε)`; remainder `a−b·q` — **approximate, non-negative domain** |
| `mat3` / `mat2` × vec | per-row `dp3` / `dp2add` (matrix uploaded as rows, same as mat4) |
| a matrix built of columns, `mat3(t, b, n)`, `mat3(m4)`, HLSL's `(float3x3)M` | its columns' registers, read as columns; a leading block of one matrix's columns, as a cast takes, that matrix's first registers as they are, columns or rows; a matrix's rows dotted as wide as the vector, so HLSL's float4x3 by rows takes `dp4`s |
| `textureLod`, `texture(…,bias)`, `textureGrad` (2D), and their projective forms | a LOD the fetch's own LOD-bias field holds (1/16 steps, -4 to 3.9375), any other through `setTexLOD`; an explicit LOD turns the computed one off; gradients through `setGradientH` and `setGradientV` (see LOD and gradients) |
| `textureProj` | perspective divide `uv·rcp(w)` then `tfetch2D` |
| `texture()` with a computed coord | coord expression emitted, then tfetch (not just bare interpolators); a coord from another sample (a dependent read) has that sample fetched first; in a pixel shader the coords' work goes first and the fetches together (see Fetch layout) |
| `CompositeInsert` (`v[i] = x`) | copy + masked component mov |
| uniform-array loop `for(i<n) sum += w[i]` | real loop; body `add acc, acc, c[base+aL]` (aL-relative array read) |
| `for(i<n){ …; if(acc.c CMP k) break; }`, `n` a bare uniform | body `setp_*` + a predicated LOOP_END (see Loops) |
| if/else in a loop body with a **constant** condition (e.g. `if(i<k)`) | unroller picks the taken arm per iteration; a `break` or `continue` in it ends the loop or the pass there (`tests/test_loop_branches.py`) |
| if/else **on data** in an unrolled loop's body | both arms each pass, merged by a select (`tests/test_loop_branches.py`) |
| `break` or `continue` **on data** in an unrolled loop's body | gated: each value keeps a stopped copy where its pixel or vertex stops, `running` and `alive` flags 0.0 or 1.0, and each pass's end a `cnd` on `running` (see Loops; `tests/test_loop_branches.py`) |
| `transpose` (mat2/3/4) | per-row column gather (swizzled movs) |
| `determinant` (mat2/3) | mat2 `ad−bc`; mat3 `row0·cross(row1,row2)` |
| `inverse` (mat2/3) | mat2 closed form; mat3 adjugate (cofactor crosses) / det · `rcp` |
| `dFdx` / `dFdy` / `fwidth` | `getTextureGradients` fetch; ddx=grad.xz, ddy=grad.yw |

## Interface conventions (GLSL ↔ Xenos)

- VS inputs → vfetch in Location order; vertex-element usage by location:
  0=POSITION, 1=COLOR, 2=TEXCOORD, 3+=TEXCOORD(idx++). Host vertex format
  must match. VS output BuiltIn Position → oPos; Location *N* → interpolator
  o*N*. FS input Location *N* → register r*N*; output Location *N*, 0 to 3,
  → oC*N*, colour target *N* (xenia:src/xenia/gpu/ucode.h:1836-1839), under
  the one `alloc colors`; past 3 is refused. The container's outputs word is
  the mask of them, bit *N* for oC*N* (XenosRecomp:XenosRecomp/shader.h:80-92),
  and SQ_PROGRAM_CNTL's ps_export_mode counts the colours from oC0 to the
  highest from its bit 1, depth in bit 0, as R600's SQ_PGM_EXPORTS_PS does
  (radeonhd:src/r600_exa.c:164): 2 for one, 8 for four, 3 for one with depth.
  At 2, the console left targets 1 to 3 as they were; Xenia never reads the
  mode (registers.h:145; xsdk:docs/HARDWARE.md, "Float, 10-bit and 64-bit
  targets and four at once").
- HLSL through glslang with `-fhlsl_functionality1` keeps each varying's semantic,
  an OpDecorateString of UserSemantic (SPV_GOOGLE_hlsl_functionality1), and xost
  takes D3D9's meaning from it where glslang left a location
  (xsdk:docs/scope/50-hlsl.md, step 4; `xsdk:src/xsc_resolve.c`;
  `tests/test_hlsl_semantics.py`):
  - A VS input's usage and index are its semantic's: D3D9's usages as libxenon
    numbers them, POSITION 0, BLENDWEIGHT 1, BLENDINDICES 2, NORMAL 3, PSIZE 4,
    TEXCOORD 5, TANGENT 6, BINORMAL 7, TESSFACTOR 8, POSITIONT 9, COLOR 10,
    FOG 11, DEPTH 12 and SAMPLE 13 (libxenon:drivers/xenos/xe.h:159-172), at
    an index to 15, which a vertex element holds in 4 bits; any other refused.
  - Outputs POSITION and PSIZE are oPos and oPts.x, DEPTH oDepth, and COLOR*n*
    oC*n*, struct members and return values too, and PSIZE an out parameter,
    which glslang makes a PointSize of its own.
  - A matrix varying takes an interpolator a column, at its semantic's index
    and those after it, as a location a column (Vulkan 1.0, "Location
    Assignment"):
    glslang gives HLSL's row *r* column *r*, so a `float3x3` at TEXCOORD2 is
    TEXCOORD2 to 4, a row each. A pixel shader reads them as its columns; a
    vertex shader writes one built of columns, and one held by rows, as a cast
    of a block's matrix, is refused.
  - Each stage packs its interpolators from 0 in the order of their semantics,
    usage, then index, D3D9's usages any of them; any other semantic, two
    variables of one, or a varying without one beside one with, refused. So a
    pixel shader declares only the interpolators it reads and a vertex shader
    exports only those it writes, as each interpolator costs a pixel time
    (xsdk:docs/HARDWARE.md, "xost's code quality, milestone 45"). Each
    interpolator record carries its semantic, usage in bits 4 to 7 and index in
    0 to 3, and bit 31 marks it a semantic's (XenosRecomp:XenosRecomp/shader.h:
    64-70 names the low 12 bits).
  - The two stages link by semantic when a title pairs them, as D3D9 links
    a pair: xsdk_shader_link (xsdk:src/shader.c) points each of the vertex
    shader's exports at the pixel shader's interpolator for its semantic, makes
    those the pixel shader doesn't read write nothing (no write mask, at
    interpolator 0, as xenia:src/xenia/gpu/ucode.h:2041-2059 has it), and sets
    SQ_PROGRAM_CNTL's export count to the pixel shader's interpolators, through
    `xsdk_xsc_link` and `xsdk_xsc_link_export` (xsdk:include/xsdk/xsc.h). An
    interpolator the pixel shader reads and the vertex shader doesn't write is
    undefined, whatever the GPU holds there, unverified on the console
    (`tests/test_hlsl_link.py`).
  - VPOS and VFACE come from the pixel parameters (see the table and Facing).

  GLSL, which has no semantics, keeps its locations, and a GLSL pair needs no
  link; one stage of each is refused.
- With the optimiser, a vertex input or a texture sample nothing exported, killed
  or tested reads takes no fetch (`xsdk:src/xsc_optimize.c`, remove_dead). The
  input's vertex element goes with its fetch, so a host's layout needn't hold it,
  and an element it holds anyway is ignored (`xsdk:src/shader.c`,
  `xsdk_shader_set_vertex_layout`); a sampler keeps its constant and resource, so
  a host binds what it bound before. setTexLOD, the gradients' setters and
  `getTextureGradients` stay.
- With the optimiser on, a vector built from a VS input's own components,
  each in its lane, and 0 or 1 in lanes past the input's, as
  `vec4(in_pos, 1.0)` of a vec3, is the input itself: its fetch's
  destination swizzle writes the 0 or 1 (xenia:src/xenia/gpu/ucode.h:677-692),
  which xsdk's and libxenon's patches for the host's layout keep, as they
  rewrite only the selectors of x to w (xsdk:src/shader.c, picked_swizzle;
  libxenon:drivers/xenos/xe.c:902-917). A lane the input has, or one another
  such vector filled with the other constant, stays an ALU copy
  (`tests/test_fetch_fills.py`).
- Bare uniforms → constant registers by Location; scalars read the register's
  `.x`.
- Uniform and push constant blocks → the block's raw std140 bytes in
  consecutive registers, for a host that copies them in as they are. Push
  constants take the first registers (after any bare uniforms), then each
  uniform block in (DescriptorSet, Binding) order. A member at byte offset *o*
  of a block at register *b* sits in *b* + *o*/16 from component (*o*%16)/4.
  Matrices keep their memory layout: column-major ones (glslang's default) hold
  a column a register, so `M * v` lowers to multiply-adds over columns.
- A block's array read at a run-time index takes `a0` from the index: one
  index a chain, into an array on 16-byte lines, `int(f)` for `f >= 0` as
  indices are (a0 is `floor(f)`), and the result within -256 to 255
  (xenia:src/xenia/gpu/ucode.h:1138-1150). a0 doesn't survive an alloc on
  the console, so a read at the same index past one loads a0 again first,
  with the instruction that loaded it before
  (xsdk:docs/HARDWARE.md, "a0 doesn't survive an alloc"). So does a read past
  a pixel shader's predicated if/else arms where an arm loaded a0 at another
  index, as a0 then holds what the arm that ran loaded (xsdk:docs/HARDWARE.md,
  "a0 and the predicate's execs").
- A vertex shader that reads gl_InstanceIndex (Vulkan's InstanceIndex), or
  any compiled with the `instanced` option (`XsdkXscOptions`, `xsc/native.py`),
  is lowered for instanced draws (xsdk:docs/scope/47-instancing.md). Its
  instance group is the float constant register past the blocks, before the
  literals, so no uniform block the GPU may load shares its group of 4. The
  runtime writes it before each piece of a draw: x 1/S, y 0.5/S (0 for S a
  power of two), z S and w the piece's first instance, S the vertices an
  instance auto-indexed or the copies' step of a repeated index buffer. The
  compiler lists it among the resources, kind `XSDK_XSC_INSTANCE`
  (`include/xsdk/xsc.h`), "instance" in `xsc/native.py`, and the container's
  constant table names it `_instance`, which marks the shader instanced. An
  exec of ALU alone before the fetches works out k = floor(r0.x * c.x + c.y),
  t.x = r0.x - c.z * k, the vertex, and t.y = k + c.w, the instance, as the
  console drew it (xsdk:docs/HARDWARE.md, "Instancing on the console,
  milestone 47 step 1"); the fetches leave r0 as it was. Every vertex fetch
  reads t.x, its source swizzle, bits 30 and 31 of its first word, 0, so the
  runtime points one at the instance's rate at t.y by writing 1 there
  (xenia:src/xenia/gpu/ucode.h:758). gl_InstanceIndex is t.y. A vertex
  shader's other built-in inputs, as gl_VertexIndex, aren't streams, and
  loading one is refused.
- gl_FragCoord → the pixel parameters, which the container turns on: its
  record's SQ_PROGRAM_CNTL has PARAM_GEN, and its SQ_CONTEXT_MISC has
  PARAM_GEN_POS, the register past the interpolators, where the GPU writes the
  pixel's column and row (xenia:src/xenia/gpu/registers.h:140, 158-234), and
  SC_OUTPUT_SCREEN_XY, without which x read 0 on the console
  (vulkan4xenon:results/2026-10-12-milestone-11-step-3/). xsdk takes both
  registers from the record as they are. gl_FragCoord.xy is the parameters'
  |xy| plus pixel constant c255, `XSDK_XSC_FRAG_COORD_CONSTANT`
  (`include/xsdk/xsc.h`), FRAG_COORD_CONSTANT in `xsc/native.py`, not a pooled
  0.5: in a tiled pass the parameters give the row within the band
  (xsdk:docs/HARDWARE.md, "Tiling's registers and packets, milestone 44 step
  1"), and xsdk writes c255 before each band runs the pass's recording, as 0.5
  and 0.5 more than the band's top row, and keeps it at 0.5 and 0.5 otherwise
  (xsdk:include/xsdk/gpu.h, tiled passes). It isn't a need or a resource:
  every shader shares the register. xsdk writes c252 to c255 as a whole group
  of 4, so a shader reading gl_FragCoord whose constants reach c252 is refused
  (`tests/test_frag_coord_band.py`). HLSL's VPOS, the pixel's whole column and
  row, gl_FragCoord less 0.5, adds c255's z and w, which xsdk keeps at 0 and the
  band's top row (`tests/test_hlsl_semantics.py`).
- gl_PointSize → oPts (export 63)'s x, the point's full width in pixels, its
  y and z, the edge and kill flags, unwritten
  (xenia:src/xenia/gpu/ucode.h:1813-1834), with SQ_PROGRAM_CNTL's
  vs_export_mode 2, a sprite's second position vector, in the record
  (xenia:src/xenia/gpu/registers.h:144, xenos.h:790-800). Without it, the
  record's mode stays 0 and points are PA_SU_POINT_SIZE's.
- gl_PointCoord → the pixel parameters' |zw|, which hold a point sprite's
  coordinates, 0 to 1 from its top left (xenia:src/xenia/gpu/registers.h:184-212,
  dxbc_shader_translator.cc:735-748); gl_FragCoord, gl_FrontFacing and
  gl_PointCoord share the register.
- gl_ClipDistance → Xenos's user clip planes, which clip in clip space against
  the exported position itself, keeping dot(position, plane) >= 0
  (xenia:src/xenia/gpu/registers.h:539-544, dxbc_shader_translator.cc:1033-1053).
  So a distance written as `dot(gl_Position, plane)`, either way round, the
  position read back from gl_Position or the value stored there, through
  locals too, and the plane a whole vec4 of a block at a fixed index, costs no
  instruction: `lower_ex`'s manifest gives `("clip", n, register)` for each
  distance n, and xsdk's need `XSDK_XSC_CLIP_PLANE`, for the host to set plane
  n from that register (xsdk's `xsdk_gpu_set_clip_planes`). Xenos has six
  planes and no cull distances. gl_PerVertex's members other than gl_Position
  are stored through it too; before, the last such store was taken for the
  position.
- gl_FragDepth → export 61's x (xenia:src/xenia/gpu/ucode.h:1842,
  dxbc_shader_translator.cc:1591-1594), with SQ_PROGRAM_CNTL's ps_export_mode
  3 in the record, where libxenon's 2 is colour alone: with 2 the console drew
  nothing of such a shader, and with 3 its depth reached the depth test
  (xsdk:results/2026-10-16-depth-export/). xsdk turns hierarchical depth off
  for a pixel shader whose record has it.
- The execs keep addressing mode 0 when ALU instructions read at a0: a pixel
  shader with its execs' mode 1 drew black on the console (the same results).
- Samplers (UniformConstant) → with a DescriptorSet decoration (Vulkan), fetch
  constants from 0 in (DescriptorSet, Binding) order; without (OpenGL), fetch
  constant = Binding.
- HLSL binds as D3D9 does (xsdk:docs/scope/50-hlsl.md, step 6;
  `tests/test_hlsl_binding.py`). glslang keeps `register(sN)` as binding N,
  and gives a sampler without one binding 0
  (glslang:glslang/HLSL/hlslParseHelper.cpp:6402-6412), so an HLSL sampler's
  fetch constant is its binding, as `SetTexture(N)` binds it; one sharing a
  binding takes the lowest no binding names. glslang packs bools and ints
  into `$Global` beside the floats, and drops `b#` and `i#`
  (hlslParseHelper.cpp:6396-6431); glslang-xenon, xsdk's glslang, keeps
  them under `--hlsl-dx9-compatible -fhlsl_functionality1` as a member's
  UserSemantic, `register(b3)` or `register(i2)`, beside any semantic it has
  (step 7; `tests/test_hlsl_registers.py`). So:
  - a bool or int in a register another member holds, as i10's `b0` and
    `i0` over `LightColor[0]`'s c0, gets a register of its own past the
    blocks; one alone in its register stays, as `register(cN)` puts it.
    Read as a value, it is that register's float, 1.0 or 0.0 for a bool,
    whatever its `b#` or `i#`: a title sets it by name.
  - a bool a pixel shader's if reads, where the if's arms go in execs, is a
    boolean constant from 128, libxenon's pixel half
    (libxenon:drivers/xenos/xe.c:2038-2064), the arms in `cexec b`*n* and
    `cexec !b`*n* execs; an arm there using a0 is refused. At `b`*N* it is
    128 + *N*, where `SetPixelShaderConstantB(N)` puts it; without a
    register, the lowest no other bool and no member's `b#` takes.
  - an int a loop counts is the loop constant that loop reads, set to
    `xsdk_gpu_loop_constant(n, 0, 1)`; a loop from other than 0 by 1 is
    refused, as a title setting the int by name can't count its passes. At
    `i`*N* it is 16 + *N* in a pixel shader and *N* in a vertex shader, the
    halves loops take below; without a register, the lowest no other count
    and no member's `i#` takes.
  - refused: a `b#` on other than a bool, an `i#` on other than an int, two
    members in one register, and a bool or int read as a boolean or loop
    constant past its stage's 128 or 16.
- The constant table (XenosRecomp:XenosRecomp/constant_table.h) is GLSL's as
  before: samplers `tex`*slot*, uniforms as vectors. HLSL's names each sampler
  and member as declared, its register set, index and count, and its type:
  class, bool, int or float, HLSL's rows and columns, array elements, a matrix
  `column_major` by columns; a bool read as a boolean constant is in the bool
  set, an int a loop counts in the int4 set at its loop constant, and either
  read as a value in the float4 set too. `xsdk_shader_constant` finds one by
  name (xsdk:include/xsdk/shader.h); `xsc/native.py`'s `constant_table` lists
  them.
- A sampler2DShadow's comparison → a register past the blocks, before the
  literals, named `_compare<slot>`, its resource `comparison` (the C's
  `XSDK_XSC_COMPARISON`). A Vulkan shader takes its compare op from the
  sampler bound at run time, so the runtime writes the op's weights there:
  1 or 0 for the reference below the depth, equal to it and above it, then 0,
  VkCompareOp's bits 0 to 2. The comparison runs in the ALU after the fetch,
  which works whatever the hardware does: whether Xenos compares in the fetch
  hasn't been checked on the console (vulkan4xenon:docs/ROADMAP.md, milestone
  23). It compares the filtered depth, so a linear filter blends depths, not
  results.
- An ALU instruction reads one constant register; a second goes through a temp.

## HLSL

Shader model 3 HLSL, vs_3_0's and ps_3_0's, as D3D9 ports bring it (xsdk:docs/scope/50-hlsl.md): glslang reads it to
SPIR-V, and xost takes D3D9's meaning from what glslang keeps of it. All 157 of milestone 50's corpus and its 21 idioms
compile (xsdk:results/2026-10-20-milestone-50/). Shader model 4 and later, D3D9's assembly shaders and Effects files
(`.fx`) are left out; a port using Effects compiles each technique's entry points itself.

- **glslang:** glslang-xenon (github.com/xenosworks/glslang-xenon, branch `xenon`; xsdk:docs/GETTING_STARTED.md,
  section 4), Khronos's 16.3.0 with HLSL's `fmod` truncating and `b#` and `i#` kept, built with SPIR-V's optimiser,
  whose legalisation glslang runs on all HLSL (16.3.0's SPIRV/GlslangToSpv.cpp:12045-12051). Its GLSL is Khronos's.
  Khronos's glslang reports the same version, so nothing tells the two apart, and builds the same HLSL with `fmod`
  floored and every bool and int at the lowest constant left.
- **Flags:** `-D -V --hlsl-dx9-compatible -fhlsl_functionality1 -S <stage> -e <entry>`, as xsdk's
  `tools/build_shaders.py` runs them and `tools/check_hlsl.py` checks them. `--hlsl-dx9-compatible` makes a `sampler2D`
  a texture and sampler in one, which without it is a bare sampler; `-fhlsl_functionality1` keeps each varying's
  semantic, and a `$Global` member's `register(bN)` or `register(iN)`, as UserSemantic strings
  (SPV_GOOGLE_hlsl_functionality1).
- **Names:** `<name>_vs.hlsl` is a vertex shader and `<name>_ps.hlsl` a pixel shader, its entry `vs_main` or `ps_main`
  where the source has one, else `main`. In build_shaders.py's header `quad_ps.hlsl` is `quad_ps_hlsl`.
- **D3D9's semantics** (Interface conventions): a vertex input's usage and index are its semantic's; POSITION, PSIZE,
  COLOR*n* and DEPTH outputs are the position, the point's size, target *n* and the depth, struct members and return
  values too; VPOS is the pixel's whole column and row, and VFACE +1 on a front face, clockwise as on D3D9, and -1 on a
  back one, as the console drew them (xsdk:docs/HARDWARE.md, "HLSL on the console, milestone 50"); each stage packs
  its interpolators by semantic, and `xsdk_shader_link` links a pair by semantic as D3D9 does.
- **Binding as D3D9 binds** (Interface conventions; `tests/test_hlsl_binding.py`, `tests/test_hlsl_registers.py`):
  - `register(cN)` is float constant *N*, as `SetVertexShaderConstantF` and `SetPixelShaderConstantF` set it: glslang
    puts the globals in one `$Global` block at their registers' offsets, which xost lays from c0.
  - `register(sN)` is texture fetch constant *N*, as `SetTexture(N)` binds it.
  - A bool a pixel shader's if reads, its arms in execs, at `register(bN)` is boolean constant 128 + *N*, where
    `SetPixelShaderConstantB(N)` puts it (libxenon:drivers/xenos/xe.c:2051-2063). A vertex shader's if and a `?:`
    read it as a value.
  - An int a loop counts at `register(iN)` is loop constant 16 + *N* in a pixel shader and *N* in a vertex shader, set
    to `xsdk_gpu_loop_constant(n, 0, 1)` for *n*; a title's build needs nothing else for it.
  - A bool or int read as a value is a float, 1.0 or 0.0 for a bool, at its own `register(cN)` or in a register past
    the blocks: a title sets it by name.
  - A bool or loop count without a register takes the lowest constant no other takes and no member's register names.
  - The constant table names every sampler and member as declared, with its register set, index, count and type, and
    `xsdk_shader_constant` (xsdk:include/xsdk/shader.h) finds one by name.
- **`fmod`** is `x - y * trunc(x / y)`, with the sign of x, as HLSL defines it, where glslang made it GLSL's floored
  `mod` (glslang-xenon's `decomposeIntrinsic`, glslang/HLSL/hlslParseHelper.cpp, and its constant fold,
  glslang/MachineIndependent/Constant.cpp; `tests/test_hlsl_fmod.py`). HLSL's `%` is as it was.

Refused, each by name, with xost's message where it's short:
- **An if in a pixel shader without a loop,** where spirv-opt keeps it a branch, as it does an `if` that changes a
  local or returns, `if (c) x *= 0.5;` among them: its values meet in Phis, which the pixel shader's if/else lowering
  doesn't read (`if/else arm does not assign o_color`). A `?:`, `clip` and an `if` around `discard` compile, and with a
  loop anywhere in the shader the unroller takes such an if (Control flow).
- **Loops** (Loops): a do/while's header Phi read after the loop; a value a loop's test works out, kept past the loop or
  read in its pass where xost can't work it out again; a value a do/while's pass works out read after the loop that no
  header Phi carries; a merge Phi of a header Phi that can't share its variable; a Phi on the edge from a test into the
  pass; a switch, which spirv-opt's inliner makes of a helper's early return; and a loop an int counts from other than
  0 by 1, as a title setting the int couldn't count its passes.
- **Semantics:** a vertex input's or interpolator's semantic other than D3D9's usages at an index to 15 (`not one of
  D3D9's declaration usages`); a matrix varying whose columns run past index 15; two varyings of one semantic (`share
  semantic`); a varying without a semantic beside one with; more than 16 interpolators.
- **A static or local with an initializer,** which xost would drop (`a Function or Private variable with an
  initializer`); glslang writes a static's value as a store, so none has reached xost.
- **Arrays and matrices:** a local array other than a constant one at a run-time index, or one stored by element; a
  matrix output held by rows, as a cast of a block's matrix, where an interpolator takes a column; a vertex shader's
  matrix input; a column of a matrix value other than a cast's.
- **Integers:** an int or bool loaded other than from a local, an input or `$Global` (`integer Load must be a foldable
  local`); and int operations past add, subtract, multiply, divide and remainder, the bitwise ones and shifts, which
  Xenos lacks (`unsupported integer op`).
- **Registers:** a `b#` on other than a bool, an `i#` on other than an int; two members in one register (`share
  register`); a bool an if reads past b127 or an int a loop counts past i15 (`past the stage's`); and an arm of a
  bool's if, in execs on its boolean constant, that uses a0 (`an arm on a boolean constant that uses a0`).
- **What GLSL can't do either** (Supported; Not yet / out of scope): a `discard` or `return` in a loop, a fifth nested
  hardware loop, more than 255 passes of one, more than 64 registers.

Wrong without a refusal, for GLSL too: `pow` of an exponent whose components differ takes its x in every lane.

## Literal constant pool (the host-upload contract)

The Xenos has no ALU float immediates, so GLSL literals are allocated
constant registers **after** the uniform-declared constants.
`lower_ex()` returns a **manifest** `[(register, (f0,f1,f2,f3))]`; the host
must upload those values to the matching shader stage before drawing:
`Xe_SetPixelShaderConstantF` for fragment manifests and
`Xe_SetVertexShaderConstantF` for vertex manifests, matching uniforms.
Example: `mix(tex, color, 0.5)` →
`add r3, color, -tex; mad oC0, r3, c0, tex` with manifest
`[(0, (0.5,0.5,0.5,0.5))]` — upload 0.5 to c0.

`0.0`/`1.0` are folded out of the manifest where there is an equivalent
no-literal encoding: algebraic identity operands (`x+0`, `x*1`, `x/1`), unit
axis `dot()` constants (a source swizzle/extract), and final export lanes
(`oC0.z = 0.0`, `oC0.w = 1.0`) via Xenos export constant-result masks. General
ALU sources do **not** have `k0/k1` source components, so uses such as
`max(x, 0.0)` still need a pooled literal unless a separate algebraic lowering
proves an equivalent form.

## Scalar pipe (the transcendentals)

Xenos `rsq`/`sqrt`/`exp2`/`log2`/`sin`/`cos` are **scalar-pipe** ops: the ALU
instruction's vector half is a nop (`vector_write_mask=0`) and the scalar
half reads one component of src3 and broadcasts its result. `normalize`,
`length`, `pow`, etc. compose these with `dp3`/`dp4` and `mul`. No common-
subexpression elimination yet, so `normalize(v)` + `length(v)` recomputes
`dot(v,v)` twice — correct, slightly redundant.

## Approximate builtins (documented accuracy / range)
- **`sin`/`cos`/`tan`** — the bare hardware `kSin`/`kCos` are accurate only
  near `[-π,π]`; arbitrary arguments need a `frac`-based range-reduction
  prologue (math in `docs/builtin-lowering.md`). `tan = sin·rcp(cos)`.
- **`asin`/`acos`** — Abramowitz-Stegun sqrt form, max error ~7e-5 over
  `[-1,1]`. `acos = π/2 − asin`.
- **`atan`** — odd polynomial, accurate on `[-1,1]` (no range reduction for
  `|x|>1` yet; `atan2`/wide-range is a refinement).

## Register budget

A shader may use only its kind's share of the shader unit's register split,
which the runtime sets in `SQ_GPR_MANAGEMENT`; past it the GPU silently
**hangs** on hardware (oracle can't see this). libxenon's set-up leaves pixel
shaders **16 GPRs** (r0..r15) and vertex shaders 112; xsdk's leaves each 64,
the kernel's own split, and a pixel shader using 64 ran on the console there
(xsdk:docs/HARDWARE.md, milestone 24). `xasm.program.RegisterBudget` names a
split: `LIBXENON_REGISTERS` by default, `XSDK_REGISTERS` for xsdk's titles
(`lower_ex(module, XSDK_REGISTERS)`, or `--pixel-registers 64
--vertex-registers 64` on `xsc.lower` and `xasm.textasm`). A shader past its
share raises `RegisterBudgetError`.

The program builder runs a **linear-scan register allocator** when it finishes
every program xsc lowers (`xsdk:src/xsc_program.c`), and `xasm/program.py` on a
hand-built one only when it needs >16
registers (pixel) or >64 (vertex, the most an instruction names), so those
compile byte-for-byte as before: it recycles dead temporaries (live range =
first def → last use), pins the read-only interpolator inputs and pixel
parameters and a vertex shader's vertex index in r0, keeps a temp live across a
hardware loop's back edge for the whole loop, and packs everything into the
minimum register count (e.g. `invtrig` 33→6, `mathfns` 20→6). Fewer registers a
thread let the GPU keep more threads in flight: packed, a pixel shader taking
four samples went from 12 registers to 6, and sampled 1.6-1.8 times as fast on
the console. A self-check asserts no two
simultaneously-live temps share a register. xsc numbers a temp for each value,
so its programs hold whole registers in their instructions' fields, encoded
once the allocator has packed them (`whole_sources`, `XSC_ABS_SOURCE`).
`tests/test_register_alloc.py` checks a vertex shader of 145 temps on
`xdis/simulate.py`, which runs a program's ALU, fetches and exports on the CPU.

## Fetch layout

Before allocation, the builder's `group_stage` (`xsdk:src/xsc_program.c`) lays out each block of
a pixel shader xsc lowers: the work its fetches' coordinates need as soon as
it's ready, then every fetch that's ready, again for fetches reading what
others fetched, then the rest in its own order. A block whose ALU work runs
into two fetches or more starts an exec wherever ALU and fetch runs meet, and
the allocator keeps what a run of fetches reads till the run ends, so no fetch
in it writes another's coordinates. Before, xsc emitted each sample's
coordinate work, then its fetch, and packing let the coordinates share one
register: four samples a texel apart ran 10.2 texels a clock on the console
that way, and 15.9 laid out (the texture units' 16; xsdk:docs/HARDWARE.md,
milestone 36). Shaders whose fetches read only their interpolators or pixel
parameters lower byte for byte as before, and so does a lone fetch after its
coordinate's work. `tests/test_fetch_layout.py` checks the layout, the
simulator's results and those shaders. No register links a read at `a0` to
the load before it, so the layout keeps them in order: a read at `a0` stays
after every `a0` load before it, and a load after every read and load before
it (xsdk:docs/HARDWARE.md, "a0 through fetch grouping";
`tests/test_dynamic_index.py`).

The ALU and the fetches run apart on the console, and an exec boundary
doesn't make one wait for the other, so the builder sets an instruction's
serialize bit where it reads across them: an ALU instruction reading or
writing a temp a fetch wrote since the last serialized instruction, in any
exec, and a fetch reading one any instruction wrote (`xsdk:src/xsc_program.c`,
exec_entry and control_flow; xsdk:docs/HARDWARE.md, "Serialized fetches" and
"Slow fetches across execs"; a fetch after a fetch, see Vertex textures). A
loop's body starts with what any instruction in it writes marked, what was
marked before a loop stays marked past it, and code on the predicate may not
run, so what was marked before it stays marked after it. Unserialized, the
ALU read a fetched index too soon in the fetch's exec, and a slow fetch's
result in a later one. `tests/test_dependent_fetch.py` checks it.

## Loops (unrolling + mem2reg)

glslang lowers `for`/`while` to mutable-memory form: an `OpLoopMerge` header,
loop-carried values held in Function-storage `OpVariable`s mutated by
`OpStore`, and an integer induction variable. xsc's unroller
(`xsdk:src/xsc_unroll.c`) runs as a pre-pass **whenever a module contains a
loop** and rewrites the whole function body into flat SSA by symbolically
executing the structured CFG. Each loop goes one of three ways:

- **A count the unroller works out,** from the induction variable's constant
  init, comparison and step, unrolls: the body is replicated that many times,
  to 4096 passes (`MAX_TRIP`), past which the loop is refused. The induction
  variable is **constant-folded**, so `float(i)`, `i*k`, `float(i)*0.2`, etc.
  become literals (no integer ALU reaches the GPU).
- **A count an int in a push constant or uniform block holds, or data,** keeps
  a hardware loop (Loops a block counts).
- **A count a bare uniform holds,** which only OpenGL's GLSL has, takes a
  lowering of its own (below).

glslang's HLSL arrives otherwise: glslang always runs spirv-opt's
legalisation on it, which leaves each loop in SSA form, Phis and the test in
its header, the counter's step in the block branching back, and values read
across blocks. A pass before the unroller (`xsdk:src/xsc_ssa.c`) puts such a
module in the form above; one whose loop headers are a label, a merge and a
branch alone, as glslang writes GLSL's, it leaves as it is:

- each Phi a Function variable, stored on each edge into its block and loaded
  where it was; a loop merge's Phi of a header's Phi from the test shares that
  Phi's variable, stored at each break, as GLSL's one local;
- the header its merge and a branch alone, what it held in a test block after
  it, and a continue block of its own taking the branch back, a counter's step
  `i + k` or `i - k` moved there, as GLSL's `for` has it;
- a value the test block works out, which the unroller doesn't walk in a loop
  it unrolls or counts, worked out again where it's read; one a do/while's
  pass works out, read after the loop, from the variable of the header's Phi
  that carries it.

Refused: a do/while's header Phi read after the loop, a value the test block
works out kept past the loop, one a do/while's pass works out read after it
that no header Phi carries, a merge Phi of a header Phi that can't share its
variable, and a switch, which spirv-opt's inliner makes of an early return
(`tests/test_hlsl_loops.py`, each shape beside its GLSL twin).

In an unrolled loop:

- **mem2reg**: every Function-local variable is threaded through an environment,
  so loop-carried accumulation *and* post-loop read-modify-write (`c *= k;`)
  resolve to SSA values — the lowering's single-store-per-variable model never
  sees the multiple stores.
- **Nested** loops expand innermost-first; **`while`** (increment in the body)
  and **`for`** (increment in the continue block) are both handled.
- **An if/else** on a condition the unroller folds, such as `if (i < k)`, takes
  the arm it picks each pass; one on data runs both arms each pass and merges
  them with selects (Control flow).
- **A `break` or `continue` the unroller folds,** such as `if (i == 2) break;`,
  ends the loop or the pass where the walk meets it. Before milestone 46 the
  walk dropped it and ran every pass.
- **A `break` or `continue` on data** is gated (xsdk:docs/scope/46-loops.md,
  "The design"). The pass carries two flags, 0.0 or 1.0 as xsc holds bools:
  `running` while the pixel or vertex runs this pass, and `alive` while it's
  still in the loop. At a break or continue on `c`, each value the loop carries
  keeps a stopped copy, `cnd(running && c, value now, stopped)`, `running`
  becomes `running && !c`, and a break clears `alive`. At the pass's end each
  value is `cnd(running, value at the end, stopped)`, and the next pass's
  `running` starts from `alive`, so the values of a pixel that left never
  change again. The arms of an if/else on data carry the flags as they carry
  locals, and the counter, read after a loop left on data, holds the pass it
  broke at. It's all straight-line code, which the optimiser and
  `xdis/equivalence.py` take as they are (`tests/test_loop_branches.py`; on the
  console, xsdk:docs/HARDWARE.md, "Breaks and continues on data in unrolled
  loops, milestone 46 step 2").
- **Refused:** a `discard` or `return` in a loop, and a `continue` on data
  where the counter isn't what it is at the pass's end, as in a `while` that
  steps it after the `continue`.

A loop a bare uniform counts, `for(i=0;i<u_count;i++)`, which doesn't unroll,
goes to a tiny register-machine lowerer (`xsdk:src/xsc_lower_loop.c`): it
initializes a persistent GPR, wraps the body in `begin_loop/end_loop`, and
returns a manifest entry `("loop", id, name)` so the host uploads
`integer_constants[8+id]`. It takes `sum += const` (gallery G28,
console-pending), **`sum += w[i]`**, a uniform array indexed by the loop
counter, lowering to the aL-relative `c[base+aL]` read (the ISA primitive
hardware-proven as G29), and `if(cond) break;`, a body `setp_*` and a
predicated LOOP_END (the G30 primitive, hardware-proven). See
`loop_uniform.frag`.

Because the pass is a no-op when there is no loop, loop-free shaders still lower
byte-for-byte as before. Unrolled shaders then go through the normal pipeline,
including the register allocator (e.g. `loop_stair` 17→3 GPRs). See `loop_*` in
`xsc/testdata/` and the `LOOP` gallery slot.

### Loops a block counts

A Vulkan shader has no bare uniforms, so the shape above never comes from one;
its loops count to an int in a push constant or uniform block, or to data. The
unroller (`xsdk:src/xsc_unroll.c`) keeps such a loop for a hardware loop:
`for (int i = s; i < b.n; i += k)` (or `b.n > i`), from a constant start by a
constant step in its continue block, and a loop whose count is data, a `for` to
`int()` of a float or an interpolant, a `while` or a `do`/`while` on data
(xsdk:docs/scope/46-loops.md). It walks the pass once, each local and whole
output the loop stores and held a value of before it carried as a value at the
top of the pass, and a value after the loop. The lowering
(`xsdk:src/xsc_lower_flow.c`) lays it out:

- each carried value's register from its value on entry, then LOOP_START;
- the pass worked out from the registers, what it reads from before the loop
  lowered once before LOOP_START; then each carried value's register takes its
  value at the pass's end, after all are worked out;
- `i` a float counting from 0, or one the loop carries from its start by its
  step where it starts elsewhere or steps by more than 1, as xost holds
  integers, so `float(i)` and a block's array at `i` (`movas` and
  `c[a0 + base]`) work; the loop constant's aL goes unread;
- `if (c) break;` ending a pass a `setp_*` and a predicated LOOP_END, the break
  on a value every vertex or pixel shares, as LOOP_END breaks only when all 64
  agree (xenia:src/xenia/gpu/ucode.h:372-373); an if/else on data inside the
  pass is selects.
- a `break` or `continue` on data anywhere in the pass, and that `if (c) break;`
  where a break or continue before it in the pass stopped a pixel, gated as in
  an unrolled loop (Loops): stopped copies, `running` and `alive` flags, and a
  `cnd` on `running` at the pass's end. `alive` is a register the loop carries
  from 1, made at the pass's first break on data, `running` starting each pass
  from it, so a pixel that left keeps its values while the rest of its group
  runs on. A `setp_eq` on `alive` at the pass's end sets p0 where the pixel has
  left, and LOOP_END breaks on p0, so the group leaves once none of it is left.
  `i` counts `alive` rather than 1, so read after the loop it holds the pass the
  pixel broke at. Nothing in the pass is on the predicate. The unroller tells a
  value every pixel shares as the lowering does (`xsdk_xsc_find_varying`).
- a loop whose count is data its condition as such a break, at the top of the
  pass, or at its end for a `do`/`while`, so a group leaves once every pixel's
  condition has.

The manifest gives `("block_loop", loop_id, kind, set, binding, offset_bytes,
start, step)`: the runtime reads the int `n` there for each draw and sets loop
constant `loop_id` to `xsdk_xsc_loop_passes(start, step, n)` passes,
ceil((n - start) / step) as Shader Model 3's loop counts them, from aL 0 by 1,
none for `n` at `start` or less; the constant counts to 255
(xenia:src/xenia/gpu/xenos.h:1086-1097). On the console a pixel shader's
`loop i0` ran 0, 1 and 3 passes as loop constant 0 said (xsdk:docs/HARDWARE.md,
"Shader constants: partial writes hang the GPU, libxenon's whole blocks
don't"). A loop whose count is data gives `("fixed_loop", loop_id, 255)`, the
need `XSDK_XSC_FIXED_LOOP`, whose loop constant the runtime sets to 255 passes,
`xsdk_gpu_loop_constant(255, 0, 1)`, the most it counts and Shader Model 3's
own: such a loop stops at 255 passes where GLSL would run on. A vertex shader's
loops take loop constants from 0 and a pixel shader's from 16, 16 each, one for
each count, start and step, loops whose count is data sharing one: both stages
read all 32 (ucode.h:352), and libxenon gives each stage half of the boolean
constants the same way (libxenon:drivers/xenos/xe.c:2038-2064). The optimiser
leaves a program with a hardware loop alone.

Hardware loops nest 4 deep (ucode.h:131), each level its own registers and
gating, an inner loop's `alive` starting from the `running` of the pass it's
in; the outer LOOP_START comes first and the inner LOOP_END first. One inside an
if/else on data runs for every pixel, as both arms do, merged by selects; one
inside an unrolled loop is copied once a pass, the copies sharing a loop
constant (`tests/test_nested_loops.py`).

A sample or derivative of what a pass works out is lowered in the pass, after
what it reads, its fetches serialized and laid out within the pass as elsewhere
(Fetch layout); one of what the pass doesn't work out goes before the loop. An
implicit LOD or a derivative there reads the quad's pixels in the same pass,
which GLSL leaves undefined where they leave at different passes. A LOD from
`textureGrad`'s gradients is one a 2x2 quad, from its first pixel inside the
primitive (LOD and gradients), and a pixel that left holds its values, so
gradients it worked out go on steering its quad's LOD: they want to be the same
across a quad, as derivatives of a value linear across it are
(`tests/test_loop_samples.py`).

Refused: a fifth nested hardware loop; a `discard` or `return` in a loop; a
store into part of an output in a hardware loop, which a local does instead; a
loop's condition on a block's int other than `i < b.n`; a matrix, array or
struct a hardware loop carries; and loops needing more than 16 loop constants
in a stage.

`tests/test_hardware_loop_breaks.py` runs each loop with a break or continue on
data on `xdis/simulate.py` both ways: each pixel alone, its own LOOP_END
breaking, as Xenia's translator does
(xenia:src/xenia/gpu/spirv_shader_translator.cc:986-1000), and all of them in
one group (`run_group`), an exec on the predicate running if any of them needs
it and a predicated LOOP_END breaking only when every one agrees, as the
console does (xsdk:docs/HARDWARE.md, "Loops on the console, milestone 46 step
1"); `tests/test_data_loops.py`, `tests/test_nested_loops.py` and
`tests/test_loop_samples.py` do the same for loops whose count is data, nested
loops and samples in a pass. On the console, xsdk_test's raster grids drew
every pixel as the GLSL has it, each shape compiled there as on the PC
(xsdk:docs/HARDWARE.md, milestone 46 steps 2 to 6).

## Control flow

The unroller (`xsdk:src/xsc_unroll.c`) takes an if/else on data in a vertex
shader, in a loop's body, or around a pixel shader's `gl_FragDepth`: it runs
both arms, each from the locals and outputs as they were before the branch, and
where the arms meet it gives each local and output they leave apart a select of
the condition and the two values, and each Phi there (glslang's `&&` and `||`)
the same. Outputs are kept as locals are and stored once at the end. A `break`
or `continue` in an arm is gated (Loops). The lowering
(`xsdk:src/xsc_lower_flow.c`) then picks, for each if/else:

- **Execs on the predicate,** in a pixel shader where every pixel takes the same
  arm, as its condition reads only constants (uniforms, push constants, literals), and
  an arm works anything out of its own beyond picking values or a bool: a
  `setp_*` on `a - b`, or on the bool's 0.0 or 1.0 not being 0, sets p0
  (xenia:src/xenia/gpu/ucode.h:1162-1204); the true arm's values go in `(p0)`
  execs and the false arm's in `(!p0)` execs, each into one register a value,
  every instruction in them predicated as its exec is, as Xenia says the
  compiler does (ucode.h:114-124); then the value is that register. The exec
  that sets p0 is marked PredicateClean=false. What the arms read from before
  the branch is lowered before the `setp`, an earlier if/else's arms with it.
  An exec on the predicate runs for none of a group of 64 that all fail it, so
  the arm not taken costs its execs' control flow and no instructions. An arm
  that loads a0 or reads a constant at it goes in plain execs instead, each
  instruction predicated as before: on the console a0 doesn't reach into an
  exec on the predicate, and one skipped loses the a0 it loads
  (xsdk:docs/HARDWARE.md, "a0 and the predicate's execs"). Past arms where one
  loaded a0, the next read at a0 loads it again (see the block arrays above).
  A vertex shader keeps selects: its arms in execs on the predicate hung the GPU
  on the console (xsdk:docs/HARDWARE.md, "Branches, vulkan4xenon milestone
  21"). So does an if/else whose arm holds a hardware loop, as a loop isn't an
  exec, or a sample or derivative lowered in a pass, as no fetch goes on the
  predicate.
- **Selects** otherwise: each a `cnd*` on the comparison branched on, `a - b`
  worked out once a branch, as a pixel shader's if/else has lowered since v1; a
  condition that isn't a float comparison is its 0.0 or 1.0, which `cndgt`
  tests. On data, a predicated exec runs for every vertex or pixel of 64 if any
  of them needs it (ucode.h:114-124), so arms that differ within a group run both
  either way, and selects need no predicate. An if/else inside an arm in execs on
  the predicate is selects too, as there's one predicate.

A pixel shader's other if/else keeps the fragment lowering's selects, and every
shader without such a branch lowers byte for byte as before.

- **No jumps:** Xenia documents how an exec on the predicate treats a group of
  64 but not how a predicated jump does, and the execs a jump would skip cost
  their control flow alone.
- **No boolean constants:** `cexec b#` would need the runtime to work each
  condition out on the CPU at every draw; the `setp` works it out in the shader,
  and every vertex or pixel agrees all the same.
- **Exports stay out of control flow:** each is written once, unconditionally,
  after the arms meet, so every path writes it, which a pixel shader's depth
  needs: nothing else gives the pixel a depth
  (xenia:src/xenia/gpu/draw_util.cc:498-502). `gl_FragDepth` stored on one path
  alone is refused for that; an interpolator or colour stored on one path takes
  that value on the other, which GLSL leaves undefined.
- **Refused:** a `discard` or `return` in an arm of an if/else the unroller
  merges, a `switch`, and a matrix, array or struct an if/else chooses.
- **The optimiser** leaves a program with predicated instructions alone, as it
  did before (`can_optimize`).

`tests/test_vertex_branches.py` and `tests/test_loop_branches.py` check both
layouts and every arm on `xdis/simulate.py`, which follows a program's control
flow as Xenia's interpreter does for one vertex or pixel
(xenia:src/xenia/gpu/shader_interpreter.cc:26-258); `tests/test_dynamic_index.py`
checks arms using a0 in plain execs and arms without it on the predicate.

## Not yet / out of scope
- **Bitwise (`& | ^ ~`) and dynamic shifts** — the Xenos ALU has no bit
  operations; these raise a clear compile error rather than miscompiling.
  Constant-amount shifts could be done as mul/div by powers of two (not wired).
- **`matrixCompMult`, mat4 `inverse`** — `transpose`/`determinant`/`inverse`
  cover mat2/mat3 (numerically gated by `tests/test_matrix.py`); per-element
  multiply and 4×4 inverse (register-heavy) are not lowered.
- **Runtime integer division** is float-emulated and **approximate** (a small
  bias corrects exact multiples; non-negative domain).
- **Float `/` and `mod()`** go through `rcp`, which lands a hair low for some
  divisors: on the console `mod(x, 3.0)` at 3 and `mod(x, 5.0)` at 5, 10 and 15
  gave the divisor, not 0, which GLSL's precision for a division allows
  (xsdk:docs/HARDWARE.md, "Samples in a loop's pass, milestone 46 step 6").
- **Loops:** a `discard` or `return` in a loop, a store into part of an output
  in a hardware loop, a fifth nested hardware loop, and more than 255 passes of
  one hardware loop, which Shader Model 3 hasn't either (Loops a block counts).

`dFdx`/`dFdy`/`fwidth` are **hardware-verified** (the `getTextureGradients`
fetch is benign to the pipeline -- confirmed by the DSOLID isolation slot
rendering a clean cube). A raw derivative visualization looks faceted because
screen-space derivatives are constant per triangle and discontinuous at the
shared edge; that is correct, not a geometry bug.

## LOD and gradients

A texture fetch samples at the sum of the fetch constant's LOD bias, the register LOD if it reads it (UseRegisterLOD),
and its own LOD-bias field. Where it computes the LOD (UseComputedLOD), it works it out from its coordinate's gradients,
or from the registers' if it reads them (UseRegisterGradients), and adds the sum to it
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:1498-1536). `setTexLOD` sets the register LOD from one component,
and `setGradientH` and `setGradientV` the gradients from three; they write no temp (xenia:src/xenia/gpu/ucode.h:669-674,
spirv_shader_translator_fetch.cc:521-541).
- **An explicit LOD,** `textureLod`'s, turns the computed LOD off. A constant the bias field holds goes there; any other
  goes through `setTexLOD`, a constant copied into a temp first, as fetches read temps alone. Before, a constant LOD was
  added to the computed one, and one past the field's range didn't encode.
- **A bias,** `texture()`'s third argument, keeps the computed LOD and adds to it the same way.
- **Gradients,** `textureGrad`'s, go to `setGradientH` and `setGradientV`, x and y, the z they don't use x again; the
  fetch computes its LOD from them. A cube map's gradients are refused: Xenia takes them in cube space
  (ucode.h:580-583), unchecked on the console. On the console the LOD from them is one per 2x2 quad, from its first
  pixel inside the primitive's gradients, so gradients that differ within a quad don't give each pixel its own LOD
  (xsdk:docs/HARDWARE.md, "Samples in a loop's pass, milestone 46 step 6"); `xdis/simulate.py`'s quads take the top
  left's.
- **Ordering:** a fetch keeps its place against every state fetch (`xsdk:src/xsc_program.c`, ordered), so each sample
  reads the state set for it; and a state fetch counts as writing no temp, so the allocator never takes its unused
  destination, which keeps every component (DEST_KEEP), for one.
- **No wait inside a run:** the console keeps the state per lane and shares it between the pixel groups in flight, so
  a wait between a setter and the fetch reading it lets another group's setter land first, and the fetch samples at
  that group's LOD (xsdk:docs/HARDWARE.md, "Loops on the console, milestone 46 step 1"). Fetch grouping takes a run of
  setters only with its fetch, once all of it is ready, so no work lands inside it, and the run waits at its first
  setter if any of it would (`xsdk:src/xsc_program.c`, run_end and run_waits; `tests/test_dependent_fetch.py`).

Offsets, a sample index and a minimum LOD, the image operands xsc doesn't take, are refused; they were dropped before.
`tests/test_lod_gradients.py` checks each on `xdis/simulate.py`, which keeps the state the setters write. The console
took each level xost's sequences ask for, a LOD and gradients that differ between pixel groups and a LOD in a loop's
passes among them (xsdk:docs/HARDWARE.md, "LODs, facing and vertex textures, vulkan4xenon milestone 22" and "Loops on
the console, milestone 46 step 1").

## Facing

The pixel parameters' x holds the pixel's column, its sign set on a back face, as Xenia reads `vFace` and
`gl_FrontFacing` from it (xenia:src/xenia/gpu/registers.h:224-227) and XenosRecomp makes it
(XenosRecomp:XenosRecomp/shader_recompiler.cpp:1465-1467). Column 0's x is a zero, -0.0 on a back face, so xsc takes
`rcpc` of it, which keeps the sign as -FLT_MAX, then `sge` of that and its negation: `gl_FrontFacing` is 1.0 on a front
face and 0.0 on a back one, in every component, and turns the pixel parameters on as `gl_FragCoord` does, both from the
one register. Before, xsc took the variable for an interpolator at r0.

So that `gl_FrontFacing` works where GLSL takes a bool, a pixel shader's if/else or discard whose condition is a bool
value rather than a comparison, `gl_FrontFacing` or a logical operation on comparisons, lowers on it as a value b: the
if/else to `cndgt b, true arm, false arm`, a discard where b holds to `kill_gt b, -b`, and where it doesn't to
`kill_eq b, -b`. `tests/test_front_facing.py` checks a `?:`, an if/else beside `gl_FragCoord`, and a discard on the
simulator, at columns 3 and 0 of each face. The console has to settle which face the sign marks: Xenia gives it the
host's front face, set from PA_SU_SC_MODE_CNTL's face as culling is
(xenia:src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:625).

HLSL's VFACE reads the same sign: `cndge` on the `rcpc`, +1 on a front face and -1 on a back one, D3D9's signs, from
one constant, `VFACE_FRONT` (`xsdk:src/xsc_lower_fragment.c`); the console's `hlsl: VFACE` check read +1 on a clockwise
front face (xsdk:docs/HARDWARE.md, "HLSL on the console, milestone 50").

## Vertex textures

A vertex shader samples as a pixel shader does, at the LOD it gives. glslang gives `texture()` there an explicit LOD
of 0, and a sample without a LOD or gradients is refused, as a vertex shader has no quad to work one out over
(xenia:src/xenia/gpu/spirv_shader_translator_fetch.cc:639-641). Its samples go after the vertex fetches and before the
position's alloc, as a pixel shader's go before its colours', but for one of what a hardware loop's pass works out, which
goes in the pass (Loops a block counts). Each fetches for every vertex, FetchValidOnly off, as
that bit is about a quad's pixels (xenia:src/xenia/gpu/ucode.h:558-561), and the container declares the samplers as a
pixel shader's does.
- Samplers take fetch constants from 0 in each stage, so a host using both moves one stage's: xsdk's
  `xsdk_shader_move_textures` adds a base to each texture fetch's constant.
- Two precautions, neither checked on the console: an exec's vertex cache mask ("A fifth vertex fetch") covers its
  vertex fetches, which lead it, and none of its texture fetches; and a fetch reading a temp another fetch wrote, in its
  exec or an earlier one, is serialized, as a texture fetch at an attribute's coordinate is. No shader in Dusklight's
  pack, xsc's testdata or vulkan4xenon's bundles lowers differently for either.

`tests/test_vertex_textures.py` checks the fields, the mask and the serialize bit, and the simulator's height and
colour.

## Validation
Every test shader is checked by the XenosRecomp oracle (which does the
inverse ucode→SPIR-V translation); `modulate.frag` is additionally
byte-identical to the hardware-photographed combiner. See
`tests/test_xsc_lower.py`, `tests/test_xsc_resolve.py`. The end-to-end proof
app was `app/glslcube` (cube whose VS+PS compile from GLSL at build time), in
xost's archived repo (github.com/xenosworks/xenos-toolchain, last commit 4a785c6); the SDK's titles and vulkan4xenon's run xsc's shaders on the
console now.

## Still to do

Measured on the console through the SDK: two kinds of code xsc writes run
slower than they need to. A scalar chain lowers as `.xxxx` vector ops, and a
vec4 plus a float lowers as four adds, a component each. With the optimiser
on, an op that writes one value takes its scalar form beside a vector op
where its operands are one register's, or a constant and a temp for an add or
a multiply (`mulsc`, `addsc`, `subsc`; xsdk:src/xsc_optimize.c, scalar_form);
not where they are two registers' or a max or min takes a constant, which
needs the allocator to put both in one register (`tests/test_scalar_unit.py`),
nor where a uniform's lane is the temp's, which the console reads as the temp
with itself (xsdk:docs/HARDWARE.md, "The scalar unit's constant forms,
milestone 48 step 5"); a literal is read from another lane of its splat.

Loops, left out of milestone 46 (xsdk:docs/scope/46-loops.md, "Not in it"):
- a constant count with a break on data unrolls and runs every pass, gated,
  where a hardware loop would stop once its group has left, which on the
  console costs nothing past the passes run (xsdk:docs/HARDWARE.md, "Loops on
  the console, milestone 46 step 1");
- the optimiser leaves a program with a hardware loop alone;
- one hardware loop runs at most 255 passes: two nested loops could count past
  it, and SPIR-V's iteration hints could bound a loop on data lower.
