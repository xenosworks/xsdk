# Milestone 48's baseline

What xost's code looks like before milestone 48 changes it (docs/scope/48-xost-code-quality-2.md, step 1): every
shader of the corpus compiled at xsdk `eda49f9`, the compiler as milestone 47 left it, with the optimiser off and on,
counted by `tools/check_optimize.py` as of the commit that adds this folder. Its new counts come from
`tools/needless.py`, which this README defines. `baseline.json` holds every shader's counts and the totals, which a
later run compares against with `--baseline`; `baseline.txt` is what the run printed.

## How it was made

In WSL Debian, after `make clean` and `make -j32` had built `build/host/libxsc.so`, from `xenos-toolchain/`:

```
python3 tools/check_optimize.py /mnt/z/dusklight-xenon/platforms/xenon/shaders xsc/testdata/*.spv \
    /mnt/z/xsdk-worktrees/M48/vulkan4xenon/tests/shaders /mnt/z/xsdk-worktrees/M48/vulkan4xenon/src/shaders \
    /mnt/z/xsdk-worktrees/M48/vulkan4xenon/bench/shaders --trials 100 \
    --group "Dusklight=*/dusklight-xenon/*" \
    --group "Dusklight costliest=*/4ee5b3c0*,*/1510c05a*,*/e2cfed9f*,*/85c8b022*,*/b331aaa6*,*/4a2fde48*,*/2eefbbd8*,\
*/75422301*,*/66457dda*,*/820f98f2*,*/6dd3a703*,*/46d1eea9*,*/05716a5b*,*/c2679fcd*,*/29fab4bb*,*/c0ddfd45*,\
*/93d36ce5*,*/bacc292a*,*/e5ea8b62*,*/68b11762*,*/4f939ba9*,*/251e226c*,*/35024802*,*/d8658b0e*,*/407248b7*" \
    --json ../results/2026-10-19-milestone-48/baseline.json > ../results/2026-10-19-milestone-48/baseline.txt
```

(the costliest group is one line, broken here). It took 146 s. A step's run is the same line with
`--baseline ../results/2026-10-19-milestone-48/baseline.json` and its own `--json`; it compares the corpus's totals and
each group's. The corpus, 441 files:
- **Dusklight's GX shaders,** GLSL, at dusklight-xenon `bf5a13e557`: 47 vertex and 103 pixel shaders, the programs its
  pack holds.
- **xost's testdata,** its 196 SPIR-V files.
- **vulkan4xenon's shaders** at `a920ee2`: `tests/shaders` 80, `src/shaders` 7, and vkbench's `bench/shaders` 8.

The groups: **Dusklight**, its 150 programs; **Dusklight costliest**, the 15 pixel and 10 vertex shaders that cost the
most in `probe_shader_cost`'s timing of 2026-10-19 (the scope's "Why"), each stage totalled apart.

## What each count is

Each count is on a program as xdis disassembles it, the optimiser off and on alike; the tables give it on, as titles
compile. A count is of ALU instructions unless it says otherwise, and one instruction can be in more than one count:
a single-lane `max` a range makes useless is in both.

**The values.** `tools/needless.py` walks the program in the order it runs and gives every register component a value.
Where the compiler knows the number, the value is that number: a literal from the shader's literal table, a fetch's 0
or 1 (destination swizzle 4, 5 or 6), or what an operation makes of those alone, worked out as `xdis/equivalence.py`
runs it and rounded to a float. "Known" below means this. Otherwise the value is a range, and whether it may be NaN:
from saturates, which give 0 for NaN (xenia:src/xenia/base/math.h:64-79), literals, `min` and `max`, and every other
operation over its operands' ranges as Xenia's interpreter computes it (xenia:src/xenia/gpu/shader_interpreter.cc:
287-957), with 0 times anything 0. An interpolator, a uniform, a constant at a run-time index and anything fetched may
be any number or NaN. The walk doesn't use vertex formats, and doesn't know a value times itself isn't negative.

**Walked without flow,** 47 programs: a program with control flow past `exec` and `alloc`, predication, or a register
at a run-time index has every register it reads unknown, so only literals read directly count there. They are 46 of
testdata's and vulkan4xenon's programs with hardware loops and testdata's `vk_dynamic_if.frag`; none of Dusklight's.

**Identities.** A vector `add`, `mul` or `mad`, an export or not, with an operand that is a known 0 or 1 in every lane
it writes, or any vector operation but a copy whose every operand lane is known. Each counts once, under the first
kind that fits:
- *on constants alone*: every operand known, so the compiler could write the result as a literal (`1 - 0` from a
  `mix(0.0, 1.0, a)`);
- *x + 0*, or *0 - x, a negation* where the operand left is negated: an `add` with a known 0, -0 included;
- *x \* 0*; *x \* 1*, or *x \* -1, a negation*: a `mul` with a known 0, or 1 or -1 in every lane. A multiply by
  (1, -1, 1, 1), as every `gl_Position` takes, isn't one: no operand modifier negates one lane;
- *mad by 0*: a multiplicand a known 0, so the result is the addend;
- *mad by 1 adding 0*: a multiplicand a known 1 and the addend a known 0, a copy;
- *mad adding 0, a mul*: the addend a known 0, which `try_fuse` can't fuse with an add after it
  (src/xsc_optimize.c:809);
- *mad by 1, an add*: a multiplicand a known 1 or -1.

**Mix pairs.** A `mad` whose multiplicand, in every lane it writes, is an `add` of `y` and a negated `x`, and whose
addend reads that same `x` (the same value, through copies, with the same absolute-value modifier): `mix` as
src/xsc_lower_glsl.c:182-192 lowers `x + (y - x) * a`. It counts when `a`, `x` or `y` is a known 0 or 1 in every lane,
under the first of those that is. Its two instructions are in the identities too where they fit.

**Divided lanes.** A one-lane vector `mul` that reads, in its lane, what a scalar `rcp`, `rcpc` or `rcpf` wrote
(through copies), when the `rcp`'s operand is known (*by a constant*) or another such `mul` reads an `rcp` of the same
value (*by one scalar*): `fdiv`'s lane at a time (src/xsc_lower.c:1134-1146). A one-lane division by a value nothing
else divides by is a true scalar division and isn't counted. **Rcps of constants:** scalar `rcp`s, `rcpc`s and
`rcpf`s whose operand is known, co-issued or alone.

**Single-lane.** A vector operation that writes one lane, has no scalar operation beside it, and isn't an export or a
copy (a `max` of one source with itself, clamped or not), counted by opcode. Dot products are among them, though the
scalar unit has none; the table gives apart the ones with a scalar twin: `add`, `mul`, `max`, `min`, `floor` and `frc`
(`adds`, `muls`, `maxs`, `mins`, `floors`, `frcs`).

**Range-useless.** Vector operations, exports included, that the ranges make useless, and the code only they read.
Copies aren't counted unless they clamp. Each counts once, under the first kind that fits:
- *a max* or *a min that can't change its value*: `max(x, y)` keeps `x` when the ranges say `x >= y` and neither may
  be NaN, and keeps `y` when `x <= y` (a NaN `x` gives `y` too); `min` likewise. With a saturate, the operand kept
  must lie in 0 to 1 and not be NaN as well;
- *a clamp of a value in 0 to 1*: a `max_sat` copy of a value in 0 to 1 that can't be NaN;
- *a value its range fixes*: an operation, not a copy, whose range is one number in every lane it writes, though the
  compiler doesn't know it: a `floor` of a value in 0 to 1, below 1;
- *an identity with a number its range fixes*: an identity, as above, once those numbers count as known: the `mad`
  that subtracts such a `floor` times 256;
- *code only those read*: with each of the above replaced by the operand it keeps or by its number, and each identity
  with a literal by its operand, the vector operations nothing an export, a fetch or a state-setting operation needs
  any more, and which it did need before: the `mul`s dividing by 256 that only the `floor` reads.

A wrap `(x * 255 - floor(x * 255 / 256) * 256) / 255` whose `floor` is 0 still leaves `x * 255 / 255`, which these
counts keep: it isn't `x` to the bit. A `_sat` on any other operation costs no slot and isn't counted.

**Copies by kind.** `tools/copy_kinds.py`'s kinds of every copy `check_optimize.py` counts, the optimiser on.

## The totals

The 425 shaders that compiled, optimiser on:

| | Corpus vertex, 135 | Corpus pixel, 290 | Dusklight vertex, 47 | Dusklight pixel, 103 | Costliest vertex, 10 | Costliest pixel, 15 |
|---|---|---|---|---|---|---|
| ALU | 4518 | 4225 | 3257 | 1342 | 1473 | 546 |
| Copies | 636 | 1032 | 305 | 118 | 170 | 37 |
| Co-issued | 528 | 199 | 471 | 159 | 222 | 96 |
| Identities | 28 | 389 | 0 | 332 | 0 | 124 |
| Mix pairs | 22 | 183 | 21 | 164 | 9 | 61 |
| Divided lanes | 0 | 60 | 0 | 48 | 0 | 44 |
| Rcps of constants | 5 | 87 | 0 | 62 | 0 | 50 |
| Single-lane | 1921 | 578 | 1623 | 383 | 602 | 186 |
| of them `add` `mul` `max` `min` `floor` `frc` | 873 | 402 | 713 | 271 | 294 | 128 |
| Range-useless | 0 | 34 | 0 | 32 | 0 | 32 |
| Walked without flow | 7 | 40 | 0 | 0 | 0 | 0 |

By kind, the pixel shaders:

| | Corpus, 290 | Dusklight, 103 | Costliest, 15 |
|---|---|---|---|
| Identity: on constants alone | 36 | 16 | 8 |
| Identity: x + 0 | 135 | 118 | 41 |
| Identity: 0 - x, a negation | 35 | 34 | 11 |
| Identity: x \* 1 | 7 | 5 | 4 |
| Identity: mad by 0 | 29 | 28 | 9 |
| Identity: mad by 1 adding 0 | 14 | 11 | 10 |
| Identity: mad adding 0, a mul | 131 | 119 | 40 |
| Identity: mad by 1, an add | 2 | 1 | 1 |
| Mix pair: a 0 or 1 | 30 | 30 | 11 |
| Mix pair: x 0 or 1 | 144 | 128 | 48 |
| Mix pair: y 0 or 1 | 9 | 6 | 2 |
| Divided lane: by a constant | 51 | 48 | 44 |
| Divided lane: by one scalar | 9 | 0 | 0 |
| Range: a max that can't change its value | 9 | 9 | 9 |
| Range: a min that can't change its value | 9 | 9 | 9 |
| Range: a value its range fixes | 5 | 4 | 4 |
| Range: an identity with a number its range fixes | 5 | 4 | 4 |
| Range: code only those read | 6 | 6 | 6 |

The vertex shaders' identities are all testdata's and vulkan4xenon's: 20 *mad by 1, an add* in `vk_many_live.vert`, and
one in each of vkbench's `mesh.vert` and `mesh_uniform.vert`, the matrix's last column times the fetched w of 1.
Dusklight's 21 vertex mix pairs are each `mix(1.0, inversesqrt(...), step(...))` normalising a normal, `x` 1. Of its
1623 single-lane vertex operations, 891 are dot products. The copies by kind, Dusklight's 150 and the corpus:

| | Corpus vertex | Corpus pixel | Dusklight vertex | Dusklight pixel |
|---|---|---|---|---|
| In a program the optimiser leaves alone | 278 | 689 | 137 | 0 |
| A constant into a temp | 81 | 69 | 42 | 25 |
| A constant into an export | 95 | 71 | 48 | 17 |
| A fetched value into an export | 120 | 54 | 53 | 20 |
| An input into an export | 0 | 29 | 0 | 4 |
| A computed value step 2 can't move | 54 | 103 | 25 | 45 |
| A clamp into a temp | 0 | 10 | 0 | 7 |
| Anything else | 8 | 7 | 0 | 0 |

Dusklight's 137 left alone are its three skinned vertex shaders', which read a constant at a run-time index.

**Against the scope's reading** of the 15 costliest pixel shaders: 186 single-lane operations against its 184; 32
range-useless against its about 32; and 61 mix pairs with a 0 or 1 against its about 64 identity slots, whose count
isn't recorded. The scope's 142 slots in the 10 costliest vertex shaders that pair a `sqrt` or `rcp` with a copy of a
literal aren't one of step 1's counts.

## Checked by hand

On two shaders, every new count was checked against the disassembly line by line (optimiser on; the numbers are
xdis's instruction numbers):

**Dusklight's `4ee5b3c093bea8ab.frag`,** 57 ALU instructions:
- identities 13: *x + 0* at 18, 24 and 37; *0 - x* at 16 and 47; *mad adding 0* at 19, 25 and 38; *on constants
  alone* at 21 and 49, each `1 - 0`; *mad by 1 adding 0* at 22 and 50, each `mix(0.0, 1.0, a)`'s mad; *mad by 0* at
  48;
- mix pairs 7: 16 and 17 `y` 0; 47 and 48 `a` 0; 18 and 19, 21 and 22, 24 and 25, 37 and 38, 49 and 50 `x` 0;
- divided lanes 8, each by a constant: 29 to 31 by 256 and 34 to 36 by 255 (the colour's wrap), 62 by 256 and 66 by
  255 (the alpha's); rcps of constants 8, at 28, 29, 30, 32, 34, 35, 61 and 63. 40 divides by `in_fog_zw.y` alone and
  isn't counted;
- single-lane 24: `add` at 41, 43, 47, 49, 55 and 58; `mad` at 10, 42, 48, 50 and 64; `mul` at 31, 36, 40 and 62;
  `max` at 51, 53, 56 and 59; `min` at 52, 54, 57 and 60; `floor` at 9;
- range-useless 11: the ±4 clamp of `s3_color` at 26 and 27, a product of saturated colours, so in 0 to 1; the ±4
  clamps of `s1_alpha` and `s2_alpha` at 51 to 54, a saturated alpha; the wrap of `s3_color`: its `floor` at 32 of a
  value in 0 to 0.996, the `mad` at 33 that subtracts it, and the `mul`s at 29 to 31 only that `floor` reads. The ±4
  clamps at 56, 57, 59 and 60 and the alpha's wrap at 61 to 64 add `in_cc0.a`, an interpolator, and aren't counted;
- copies 1, at 11: a computed value step 2 can't move.

**Dusklight's `f12926532ee0b4d0.vert`,** 42 ALU instructions:
- identities 0: the `mul` into `oPos` at 18 is by (1, -1, 1, 1);
- mix pairs 1: 36 and 37, `mix(1.0, inversesqrt(...), step(...))`, `x` 1;
- divided lanes 0: 29 is the only `mul` by 28's `rcp`, and 23 divides three lanes by 22's `rcp` at once; rcps of
  constants 0;
- single-lane 22: `dp4` at 10 to 12, 14 to 17, 31 and 32; `dp3` at 20, 24, 26, 27 and 39; `mul` at 25, 29 and 41;
  `max` at 30, 34 and 40; `add` at 36; `mad` at 37;
- range-useless 0: the `max`es at 30, 34 and 40 take unbounded values; 34's is a vector's dot product with itself,
  which the walk doesn't know can't be negative;
- copies 5: a constant into a temp at 13, constants into exports at 44, 47 and 50, and the fog coordinates, a computed
  value, at 51.

`tests/test_needless.py` checks each count on a hand-built program, and that the same program without its saturate
has no range-useless operation.

## Found on the way

`f12926532ee0b4d0.vert`'s `max(nrm_len2, 1e-10)` at 34 and `step(1e-10, nrm_len2)` at 35 read `c170`, which is 0:
`xsdk_xsc_literal_reg` pools literals by their components rounded to 9 places (src/xsc_lower.c:428-451), so 1e-10
shares 0.0's register. The counts take the literal table as compiled. All 21 of Dusklight's shaders that use 1e-10,
each a vertex shader normalising a normal, compile it as 0.

## On the console

Two runs of `probe_shader_cost`, built clean from main at `eda49f9` against Dusklight's committed pack, through the
console agent (`probe_baseline_1.log`, `probe_baseline_2.log`):

| | Run 1 | Run 2 |
|---|---|---|
| Pixel shaders, 103, a draw of each | 63,027 us | 63,078 us |
| Vertex shaders, 47, a draw of each | 13,369 us | 13,338 us |

- **Pictures:** each of the 103 pixel shaders' picture hashes the same in both runs, so a step that keeps results bit
  for bit must keep every hash.
- **The milestone's targets, from the runs' means:** pixel shaders at most 55,486 us, 12% under 63,053; vertex shaders
  at most 12,686 us, 5% under 13,354.

## Step 2: identities folded

Built on 2026-10-19 at `99d3731` (docs/scope/48-xost-code-quality-2.md, step 2).

- **`mix`** (src/xsc_lower_glsl.c, `mix`): where the weight is a constant 0 or 1 in every lane, a copy of `x` or `y`;
  where `x` is a constant 0, `y * a` through `xsdk_xsc_binop`; where `y` is, `mad(-x, a, x)`. A constant is one
  `xsdk_xsc_splat_const` finds the same in every lane; a vector whose lanes differ is lowered as before.
- **The optimiser's `fold_identities`** (src/xsc_optimize.c), in the first round after `fuse_multiply_adds`: an add, mul
  or mad that reads a known 0, 1 or -1 in every lane it writes, a pooled literal (`XscProgram.literals`) or a fetch's 0
  or 1, becomes what remains. `x + 0` and `0 - x` a copy, `x * ±1` a copy, `x * 0` a copy of the 0, a mad by 0 its
  addend, a mad adding 0 a mul, which `try_fuse` fuses with an add after it in the next round, and a mad by ±1 an add.
  One pass over the program folds them all on one flow, as no fold changes what an op writes. An op reading constants
  alone is left: folded, it became a constant's copy, which `propagate_copies` took into a vector's lanes a copy each,
  where `compute_in_place` places the op once (vulkan4xenon's `loop_if.frag`, 24 ALU instructions, would have taken
  25).
- **Exact:** each fold keeps the result but for a zero's sign (`-0 + 0` is `+0`). The GPU's 0 times anything is +0,
  NaN and infinities too (xenia:src/xenia/gpu/ucode.h:976-990, shader_interpreter.cc:339-346, 403-413), so `x * 0`, a
  mad by 0 and a weight of 0 keep the result as well, as `dot`'s one-hot fold already assumed. A weight of 1 gives `y`
  exactly, where `(y - x) * 1 + x` can round. A fused mad adding 0 now rounds as the GPU's mad does, where the mad and
  the add rounded apart: Xenia's interpreter rounds the product first, so the same, but the console hasn't said.
- **xsdk_test's compiler check:** `tests/xsc_shaders.h` regenerated, as 3 of its 46 shaders change.

**xost's tests:** every one passes. `tests/test_identities.py` runs `xsc/testdata/vk_identities.frag` on the simulator,
the optimiser off and on: mixes with a weight of 1 and of 0, an `x` and a `y` of 0, `x * vec4(1.0)`, `vec4(0.0) - y`,
`a * vec4(0.0)`, and a mix with a 0 and a multiply by a 1 that only the optimiser sees, through the vector they're
built into. Every export must be exact, needless.py must find no identity or mix pair with the optimiser on, and the
ALU instructions must be 24 off and 7 on. Against bd84bf9's compiler it failed at once: the weight of 1 gave
0.35000002, -0.19999999 and 0.099999994 for 0.35, -0.2 and 0.1; there the program took 28 and 18.

**The counts,** `step2.txt` and `step2.json`, the baseline's command over its 441 files with `--baseline`; 0 shaders
differ off against on. The corpus's pixel shaders are the 290 the baseline compiled: `vk_many_fetches.frag` compiles
now too (below). bd84bf9, before this step, counts as the baseline does. Optimiser on, baseline -> step 2:

| | Corpus vertex, 135 | Corpus pixel, 290 | Dusklight vertex, 47 | Dusklight pixel, 103 | Costliest vertex, 10 | Costliest pixel, 15 |
|---|---|---|---|---|---|---|
| ALU | 4518 -> 4515 | 4225 -> 4001 | 3257 -> 3257 | 1342 -> 1134 | 1473 -> 1473 | 546 -> 457 |
| Copies | 636 -> 636 | 1032 -> 1052 | 305 -> 305 | 118 -> 138 | 170 -> 170 | 37 -> 41 |
| Co-issued | 528 -> 528 | 199 -> 198 | 471 -> 471 | 159 -> 158 | 222 -> 222 | 96 -> 95 |
| Registers | 720 -> 720 | 1403 -> 1394 | 300 -> 300 | 466 -> 457 | 94 -> 94 | 124 -> 121 |
| Identities | 28 -> 3 | 389 -> 38 | 0 -> 0 | 332 -> 10 | 0 -> 0 | 124 -> 0 |
| Mix pairs | 22 -> 22 | 183 -> 4 | 21 -> 21 | 164 -> 0 | 9 -> 9 | 61 -> 0 |
| Single-lane | 1921 -> 1921 | 578 -> 513 | 1623 -> 1623 | 383 -> 320 | 602 -> 602 | 186 -> 157 |

- No shader takes more ALU instructions than at the baseline. Dusklight's pixel shaders take 15.5% fewer, its 15
  costliest 16.3%.
- **Left:** Dusklight's 10 identities are ops on constants alone, in 6 programs; the corpus's others are in programs
  the first round leaves alone (hardware loops, predicates, a0). The 21 vertex mix pairs are `mix(1.0, ...)`, an `x`
  of 1, and the 4 pixel ones a `y` of 1 or 0s and 1s that differ by lane, which fold only a lane at a time.
- The copies that rose, in Dusklight's pixel shaders clamps into temps 7 to 19 and constants, fetched values and inputs
  into exports 41 to 51, do what an add or a mad with a 0 did before.

**Containers that change,** compiled by bd84bf9 and by this step, per program in `step2_containers.txt`:
- **Dusklight:** 63 of its 150 programs, all pixel shaders, optimiser on, 4599 ALU instructions to 4391; 62 off, 8298
  to 8108. Its pack must be rebuilt.
- **vulkan4xenon:** `tests/shaders` 14 of 80 on, 1312 to 1298 (the 12 `aurora_*` that mix, `branch_data.vert` and
  `loop_if.frag`); `bench/shaders` 2 of 8, `mesh.vert` and `mesh_uniform.vert`, a mad by the fetched w of 1 now an add,
  5 instructions as before; `src/shaders` none.
- **xsdk_test's 46:** `vk_vertex_if.vert`, `vk_loop_grid.frag` and `vk_vertex_loop_break.vert`, one instruction fewer
  each.

**Found on the way:** with bd84bf9's optimiser, `vk_many_fetches.frag` was refused for 367 registers: copy propagation
rewrote its `floor`'s operand but left the second source, which a floor doesn't read, naming a copy's temp the pass
then dropped, and the allocator counts a temp read before any write as an input. Folded, the program takes another
shape and compiles in 13 registers, its exports equal off and on. The stale operand can still happen elsewhere; not
fixed here.

**Compile time:** compiling Dusklight's 150 programs on the PC, best of 25 runs in three alternations, took 78.8 to
80.2 ms before and 77.5 to 81.1 ms after. Placed before `fuse_multiply_adds`, the pass cost about 5%, a flow built in
every round that reached it; after it, only the rounds that reach `compute_in_place` pay for one.

**On the console,** still to run: xsdk_test with its compiler check; vulkan4xenon_test twice with its bundle rebuilt;
`probe_shader_cost` with Dusklight's pack rebuilt, its pictures against the baseline's (63 pixel programs change: the
same picture, or within one step of a channel where a weight of 1 or a newly fused mad rounds differently), and the
time; `probe_xsc` for the compile time, at most 1.42 s; Dusklight's title scene.
