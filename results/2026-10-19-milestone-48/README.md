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

**On the console,** built clean from the M48 pair rebased on main `ddf4569`, Dusklight's pack and vulkan4xenon's
bundles rebuilt with this xost (local/m48/step2_build.sh):
- **`probe_step2.log`:** Dusklight's pixel shaders take 56,384 us for a draw of each, 10.6% under the baseline's
  63,053; its vertex shaders 13,375 us, as before. All 103 pictures hash as the baseline's, bit for bit, the 63
  changed programs among them.
- **`probe_xsc_step2.log`:** 150 of 150 programs compile on the console as the pack holds them, and the 150 take
  1.336 s, within 1.42. It needed `tools/gen_xsc_probe.py` to read a pack's seven-field resource lines (milestone 46's
  start and step), which it had read as none, failing every program on its resource count.
- **`step2_xsdk_test.log`:** 461 passed, 2 failed, the controller's checks with no pad.
- **`step2_vulkan4xenon_test_1.log`, `_2.log`:** 492 of 492 each, the test bundle rebuilt (14 programs change).
- **`step2_dusklight.log`:** Dusklight's title scene with the rebuilt pack, at 26 to 30 fps as before.
- **Breaks:** `tests/test_identities.py` failed against the code before the step (above). No console break was built,
  as the probe compares every picture with the baseline's bit for bit.

## Step 3: division

Built on 2026-10-19 at `d6d89e2` (docs/scope/48-xost-code-quality-2.md, step 3).

- **By a constant** (src/xsc_lower.c, `constant_reciprocals` and `fdiv`): a divisor whose every lane is a constant,
  one number or one a lane, is one `mul` by a literal of the reciprocals, where it took an `rcp` and a one-lane `mul`
  a lane. Each reciprocal is 1 / c worked out in doubles and rounded to a float once, the float nearest it: 53 bits are
  more than 2 x 24 + 2, so rounding twice gives what rounding once would. Where a lane or its reciprocal isn't a
  normal float, a 0, an infinity, a NaN or a denormal, the whole division keeps the GPU's `rcp`s, as before, as the
  console hasn't said what its `rcp` gives there.
- **By one scalar:** a vector divided by one float that isn't a constant, which glslang builds as a vector of that
  float, was already one `rcp` and one vector `mul` (`scalar_construct`), and stays so. No division in the corpus reads
  one value in every lane another way (a swizzle like `u.ww`, or a vector of one lane read twice), so the step adds
  nothing there. The 13 *by one scalar* lanes the counts still find aren't `fdiv`'s: `fs_minv.frag`'s 9 are
  `inverse`'s cofactors transposed, each lane times 1 / det (src/xsc_lower_matrix.c, `inverse3`), and
  `vk_many_fetches.frag`'s 4 are two divisions by the same uniform's two lanes, which only an `rcp` shared between
  divisions would save.
- **Exact:** the reciprocal's rounding is the only change. The simulator's `rcp` gives the float nearest 1 / c, as
  Xenia's does (xenia:src/xenia/gpu/shader_interpreter.cc:786-788), so there every export is what it was, bit for bit.
  The console's `rcp` lands a hair under a third and a fifth (docs/HARDWARE.md), so on it a quotient by a constant
  whose `rcp` isn't the nearest float moves by up to an ulp, and `mod(x, 3.0)` and `mod(x, 5.0)` at a multiple of 3
  or 5 should now give 0, where they gave 3 or 5. a times the nearest reciprocal is within 1.5 ulp of a / c, inside
  the 2.5 ulp the Vulkan specification allows `OpFDiv`.

**xost's tests:** every one passes. `tests/test_division.py` runs `xsc/testdata/vk_division.frag` on the simulator,
the optimiser off and on: a vec3 and a float divided by constants (255 and 7), a vector by a constant a lane (3, -5,
0.1 and 256), a vector by one varying float, and lanes by varying floats and by a constant vector with a 0. Every
export must be a times the float nearest 1 / c, or a times `rcp(b)`, exactly; the lanes by constants within an ulp of
a / c, and not all equal to it, so the test can tell the two apart; needless.py must find 2 divided lanes and 2 rcps of
constants, those of the divisor with a 0; and the ALU instructions must be 21 off and 13 on. Against 1d14f53's compiler
it failed on its counts: 9 divided lanes, 10 rcps of constants and 34 ALU instructions with the optimiser off; on, the
program took 26. Its exports were exact there too, the simulator's `rcp` being the nearest float.

**The counts,** `step3.txt` and `step3.json`, the baseline's command over its 441 files with `--baseline`; 0 shaders
differ off against on, 50 not simulated and 15 not compiled, as at step 2. The table is over the 290 pixel shaders the
baseline compiled, as step 2's is; `vk_many_fetches.frag`, which compiles since step 2, takes 133 instructions there
and 128 now. Optimiser on, baseline -> step 2 -> step 3:

| | Corpus vertex, 135 | Corpus pixel, 290 | Dusklight vertex, 47 | Dusklight pixel, 103 | Costliest vertex, 10 | Costliest pixel, 15 |
|---|---|---|---|---|---|---|
| ALU | 4518 -> 4515 -> 4515 | 4225 -> 4001 -> 3970 | 3257 -> 3257 -> 3257 | 1342 -> 1134 -> 1116 | 1473 -> 1473 -> 1473 | 546 -> 457 -> 439 |
| Copies | 636 -> 636 -> 636 | 1032 -> 1052 -> 1053 | 305 -> 305 -> 305 | 118 -> 138 -> 139 | 170 -> 170 -> 170 | 37 -> 41 -> 42 |
| Co-issued | 528 -> 528 -> 527 | 199 -> 198 -> 131 | 471 -> 471 -> 471 | 159 -> 158 -> 96 | 222 -> 222 -> 222 | 96 -> 95 -> 45 |
| Registers | 720 -> 720 -> 720 | 1403 -> 1394 -> 1382 | 300 -> 300 -> 300 | 466 -> 457 -> 448 | 94 -> 94 -> 94 | 124 -> 121 -> 120 |
| Divided lanes | 0 -> 0 -> 0 | 60 -> 60 -> 9 | 0 -> 0 -> 0 | 48 -> 48 -> 0 | 0 -> 0 -> 0 | 44 -> 44 -> 0 |
| Rcps of constants | 5 -> 5 -> 4 | 87 -> 87 -> 7 | 0 -> 0 -> 0 | 62 -> 62 -> 0 | 0 -> 0 -> 0 | 50 -> 50 -> 0 |
| Single-lane | 1921 -> 1921 -> 1921 | 578 -> 513 -> 533 | 1623 -> 1623 -> 1623 | 383 -> 320 -> 336 | 602 -> 602 -> 602 | 186 -> 157 -> 169 |

- **This step's own effect:** Dusklight's pixel shaders take 1.6% fewer ALU instructions than at step 2, its 15
  costliest 3.9%; against the baseline, 16.8% and 19.6%. No shader takes more ALU instructions than at step 2, off or
  on.
- Every lane divided by a constant is gone, and every rcp of a constant in Dusklight. The corpus's 11 rcps of
  constants left aren't `fdiv`'s: integer division's (`fs_intdiv.frag`, `vk_dynamic_rows.vert`,
  `vk_instance_index.vert`, vulkan4xenon's `matrix_index.vert`) and `smoothstep`'s 1 / (edge1 - edge0)
  (`stepsmooth.frag`, `verify_smooth.frag`).
- Co-issued slots fell, 158 to 96 in Dusklight's pixel shaders: each of the 62 `rcp`s that went sat beside a vector
  operation, so the ALU count fell by 18. Single-lane rose, 320 to 336: a one-lane division by a constant is now a
  one-lane `mul` with no `rcp` beside it, which step 5's scalar unit is for.

**Containers that change,** compiled by 1d14f53 and by this step, per program in `step3_containers.txt`:
- **Dusklight:** 19 of its 150 programs, all pixel shaders, off and on; optimiser on 4391 ALU instructions to 4373,
  off 8108 to 8016. 18 of them divide by 255 or 15, whose `rcp` on the console may not be the nearest float;
  `2eefbbd89e53921c.frag` divides by 2 alone. Its pack must be rebuilt.
- **vulkan4xenon:** `tests/shaders` 3 of 80, off and on, on 1298 to 1296: `instance_grid.vert` divides by 40,
  `lod_gradients.frag` by 256 and `loop_sample.frag` by 64; `src/shaders` and `bench/shaders` none.
- **xsdk_test's 46:** `vk_loop_sample_grid.frag`, 171 to 160; `tests/xsc_shaders.h` regenerated. Its other divisions
  are by powers of two, and its mods by 3 and 5 are of values half past a whole, so the raster grid should draw as
  before.

**Compile time:** compiling Dusklight's 150 programs on the PC, best of 25 runs in two sets of three alternations, took
75.1 to 77.3 ms before and 73.3 to 76.0 ms after, about 1.4 ms less in each pair: the check for a constant divisor is a
lookup, and the programs have fewer instructions to optimise.

**On the console,** built clean from the M48 pair, Dusklight's pack and vulkan4xenon's test bundle rebuilt with this
xost (local/m48/step3_build.sh):
- **`probe_step3.log`:** Dusklight's pixel shaders take 55,545 us for a draw of each, 11.9% under the baseline's
  63,053 and 1.5% under step 2's; its vertex shaders 13,364 us, as before. All 103 pictures hash as the baseline's, bit
  for bit, the 18 dividing by 255 or 15 among them: whatever the quotients' rounding changed reached no 8-bit output
  in the probe's pictures.
- **`probe_xsc_step3.log`:** 150 of 150 programs compile on the console as the pack holds them, the 150 in 1.320 s.
- **`step3_xsdk_test.log`:** 461 passed, 2 failed, the controller's checks with no pad.
- **`step3_vulkan4xenon_test_1.log`, `_2.log`:** 492 of 492 each, the test bundle rebuilt (3 programs change).
- **`step3_dusklight.log`:** Dusklight's title scene with the rebuilt pack, at 26 to 30 fps as before.
- **Breaks:** `tests/test_division.py` failed against the code before the step (above). No console break was built,
  as the probe compares every picture with the baseline's bit for bit.

## Step 4: ranges

Built on 2026-10-19 at `c9c4a38` (docs/scope/48-xost-code-quality-2.md, step 4).

- **The ranges** (src/xsc_optimize.c, `fold_by_ranges`, which step 2's `fold_identities` became): walking the ops in
  the order they run, the pass gives each register component a range, whether it may be NaN and whether it may be -0,
  as Xenia's interpreter computes each op (xenia:src/xenia/gpu/shader_interpreter.cc:287-956). They come from
  literals, a fetch's 0 and 1, saturates, which give +0 for NaN and -0 (xenia:src/xenia/base/math.h:64-79), and `add`,
  `mul`, `mad`, `max`, `min` and `floor` over their operands' ranges, 0 times anything +0. An interpolator, a uniform
  and anything fetched may be any number or NaN: a texture's format is the host's to choose. Other ops, `cnd`s and
  comparisons among them, may give anything, as may scalar ops but for their clamp. Bounds are worked out in doubles
  and rounded outward to floats, a step further where the doubles rounded (a fast two-sum's error term says), so any
  rounding the GPU does lies within; an operand's denormal reads as 0 (shader_interpreter.h:108-112). The walk is one
  pass over the ops, in the first round's programs alone: no hardware loops, predication or `a0`, as before.
- **What goes:** a `max` or `min` that gives one operand for every value in the ranges becomes a copy of it, and a
  clamped copy of a value in 0 to 1 that can't be NaN or -0 loses its clamp; copy propagation takes both. Step 2's
  identities fold through numbers a range fixes too: the floor of a value in 0 to 1, below 1, is 0, so the wrap's `mad`
  that subtracts it times 256 becomes a copy, and the floor and the `mul`s only it reads go.
- **Exact:** `max` is `x >= y ? x : y` and `min` `x < y ? x : y` (shader_interpreter.cc:347-361), so a NaN gives `y`,
  and equal operands give `x` for `max` and `y` for `min`, which differ for -0 and +0. A kept `x` therefore can't be
  NaN, nor `y` where `x` is kept, and operands that may be equal keep only at a number other than 0; a clamp keeps
  nothing that may be -0. These drops keep every result bit for bit, zeros' signs too. Keeping `y` where `x` may be
  NaN rests on Xenia's D3D9 rule (xenia:src/xenia/gpu/ucode.h:975-978), which the console hasn't confirmed; none of
  Dusklight's drops needs it. The identities through fixed numbers keep the result but for a zero's sign, as step 2's
  do.
- **The simulators** (`842e73a`): `xdis/simulate.py`'s clamp kept -0, and `xdis/equivalence.py`'s kept -0 and NaN,
  where Xenia gives +0 for both. With the old clamp, the test's clamped max of -a and a at a = +0 gave -0 in both, and
  check_optimize.py found `vk_ranges.frag` differing, 1 / x -inf against +inf. Every test passes as before with step
  3's compiler.

**xost's tests:** every one passes. `tests/test_ranges.py` runs `xsc/testdata/vk_ranges.frag` on the simulator, the
optimiser off and on, at inputs on the ranges' edges and a NaN: a ±4 clamp of a product of saturated values and a
clamp of that, a `min` and a `max` of a saturated `a` with its own ends, and a 255/256 wrap of the product, each
dropped; a clamp of an input, a `min` of a value that may be NaN, the max of -a and a and its clamp, `1 / x` of it,
and, biting at their operand's top, `floor(a)`, `min(p * 2, 1)` and the floor of a product whose exact top is below 1
but rounds to 1, each kept. Every export must be the same bit for bit, the edges' lane 275, and the ALU instructions
48 off and 23 on; needless.py, blind to -0, still counts the kept max of -a and a. Against 842e73a's compiler it failed
on its counts: 32 ALU instructions with the optimiser on. Broken on purpose, it fails on its exports with NaN ignored
(a NaN where 4 belongs), bounds not rounded outward (259 for 275), a saturate's top taken below 1 (19) or a clamp of
-0 dropped (-inf for +inf), and on its counts with -0 and +0 taken as equal (22) or products widened an ulp (29).

**The counts,** `step4.txt` and `step4.json`, the baseline's command with `--baseline`; 0 shaders differ off against
on, 50 not simulated and 15 not compiled. It ran over 445 files, the baseline's 441 and testdata's four added since
(`vk_literal_pool.frag`, `vk_identities.frag`, `vk_division.frag`, `vk_ranges.frag`), so step4.txt's totals hold
them; the table is over the shaders the baseline compiled, as steps 2's and 3's are. `vk_many_fetches.frag`, which
compiles since step 2, takes 121 instructions, 128 at step 3: a ±4 clamp of a saturated product, its wrap and a clamp
of a constant go. Optimiser on, baseline -> step 3 -> step 4:

| | Corpus vertex, 135 | Corpus pixel, 290 | Dusklight vertex, 47 | Dusklight pixel, 103 | Costliest vertex, 10 | Costliest pixel, 15 |
|---|---|---|---|---|---|---|
| ALU | 4518 -> 4515 -> 4515 | 4225 -> 3970 -> 3934 | 3257 -> 3257 -> 3257 | 1342 -> 1116 -> 1086 | 1473 -> 1473 -> 1473 | 546 -> 439 -> 409 |
| Copies | 636 -> 636 -> 636 | 1032 -> 1053 -> 1052 | 305 -> 305 -> 305 | 118 -> 139 -> 138 | 170 -> 170 -> 170 | 37 -> 42 -> 41 |
| Co-issued | 528 -> 527 -> 527 | 199 -> 131 -> 131 | 471 -> 471 -> 471 | 159 -> 96 -> 96 | 222 -> 222 -> 222 | 96 -> 45 -> 45 |
| Registers | 720 -> 720 -> 720 | 1403 -> 1382 -> 1381 | 300 -> 300 -> 300 | 466 -> 448 -> 448 | 94 -> 94 -> 94 | 124 -> 120 -> 120 |
| Identities | 28 -> 3 -> 3 | 389 -> 38 -> 32 | 0 -> 0 -> 0 | 332 -> 10 -> 10 | 0 -> 0 -> 0 | 124 -> 0 -> 0 |
| Range-useless | 0 -> 0 -> 0 | 34 -> 32 -> 2 | 0 -> 0 -> 0 | 32 -> 30 -> 0 | 0 -> 0 -> 0 | 32 -> 30 -> 0 |
| Single-lane | 1921 -> 1921 -> 1921 | 578 -> 533 -> 509 | 1623 -> 1623 -> 1623 | 383 -> 336 -> 318 | 602 -> 602 -> 602 | 186 -> 169 -> 151 |

- **This step's own effect:** Dusklight's pixel shaders take 2.7% fewer ALU instructions than at step 3, its 15
  costliest 6.8%; against the baseline, 19.1% and 25.1%. All 30 instructions are in five of the costliest,
  `4ee5b3c0` 44 to 35, `e2cfed9f` 43 to 34, `75422301` and `b331aaa6` 5 each and `85c8b022` 2: ±4 clamps of saturated
  or already clamped values, and wraps' floors of values below 1. No shader takes more ALU instructions than at step 3,
  off or on, and with the optimiser off none changes.
- Every range-useless op in Dusklight is gone, 9 maxes, 9 mins, 4 floors, the 4 `mad`s that subtract them and 4
  `mul`s only those read. The corpus's 2 left are adds of a `cnd`'s lane in unrolled loops, which the ranges don't
  follow (`vk_loop_grid.frag`, vulkan4xenon's `loop_break.frag`). The other change in the 290 is `fs_vrelation.frag`,
  15 to 9: `1 - 0` on constants alone, which step 2 leaves, is now 1 to its readers, and their multiplies by it fold.
- Single-lane fell 336 to 318 in Dusklight's pixel shaders: the alphas' clamps were one lane each.

**Containers that change,** compiled by 842e73a and by this step, per program in `step4_containers.txt`:
- **Dusklight:** 30 of its 150 programs, all pixel shaders, with the optimiser on, none off; 4373 ALU instructions to
  4343. The 5 above lose instructions; the other 25 differ only in an export's swizzle of a lane it doesn't write: the
  fog's clamp of a factor already saturated goes, so its copy propagates into the export, where compute_in_place put
  it before. Its pack must be rebuilt.
- **vulkan4xenon:** none of `tests/shaders`, `src/shaders` and `bench/shaders`, off or on.
- **xsdk_test's 46:** none; `tests/xsc_shaders.h` regenerates as it is.

**Compile time:** compiling Dusklight's 150 programs on the PC, best of 25 runs in two sets of three alternations,
took 70.2 to 73.6 ms before and 71.6 to 73.2 ms after, about 1 ms, 1.5%, more on average: the walk works out every
op's ranges once in each round that reaches it. At that share the console's 1.320 s would be about 1.34 s, within 1.42.

**On the console,** built clean from the M48 pair rebased on main `8fa66d5`, Dusklight's pack rebuilt with this xost
(30 programs change) and vulkan4xenon's bundles as they are (none change; local/m48/step4_build.sh):
- **`probe_step4.log`:** Dusklight's pixel shaders take 54,145 us for a draw of each, 14.1% under the baseline's
  63,053, past the milestone's 12%, and 2.5% under step 3's; its vertex shaders 13,348 us, as before. All 103 pictures
  hash as the baseline's, bit for bit.
- **`probe_xsc_step4.log`:** 150 of 150 programs compile on the console as the pack holds them, the 150 in 1.354 s.
- **`step4_xsdk_test.log`:** 463 passed, 2 failed, the controller's checks with no pad.
- **`step4_vulkan4xenon_test_1.log`, `_2.log`:** 492 of 492 each.
- **`step4_dusklight.log`:** Dusklight's title scene with the rebuilt pack, at 26 to 30 fps as before.
- **Breaks:** `tests/test_ranges.py` failed against the code before the step and broken on purpose six ways (above);
  no console break, as the probe compares every picture with the baseline's bit for bit.

## Step 5: the scalar unit

Built on 2026-10-19 at `7dc5220`, on `f33acdb` and `4bf5d02` (docs/scope/48-xost-code-quality-2.md, step 5), and
fixed at `31e1bf6` by what the console showed: a constant form whose constant's lane is its temp's reads the temp for
both operands (below, "The first console run").

- **co_issue keeps scanning** (`f33acdb`, src/xsc_optimize.c, `co_issue`): it started its scan over after every pair;
  it now goes on past one and leaves what a pair opened to the next round. Alone, it left every container of the
  corpus byte for byte as at step 4 and took 8% off compiling Dusklight's 150 programs on the PC.
- **Scalar forms** (`scalar_form`): an op that writes one value, in one lane or the same in several, takes the scalar
  pipe beside a vector op as `adds`, `subs`, `muls`, `maxs`, `mins`, `floors`, `frcs` or `truncs` where its operands
  are one register's, and as `mulsc`, `addsc` or `subsc` where they are a constant and a temp. co_issue converts an op
  only to pair it; one left alone stays a vector op. A copy of one value is a max and pairs as `maxs`.
- **mulsc, addsc and subsc** read a constant's w and a temp's x, the temp's index spread over the opcode's bit 0,
  src3_sel and src3's swizzle (xenia:src/xenia/gpu/ucode.h:954-960, 1992-1995). In the optimiser src3 holds the temp,
  as for any scalar op, and `XscAlu.scalar_constant` the constant; xsc_program.c's `encode` spreads the index. xost's
  simulators, counts and text assembler read them since `4bf5d02`; textasm lost the constant's component before.
- **Two greedy pairings** (`pair_ops`): pairing each op with its nearest partner did best in the two skinned vertex
  shaders, `93d36ce5` and `c0ddfd45` 150 and 166 ALU instructions against 162 and 180, but cost the lit ones a register
  each; pairing scalar ops before converted ones did the reverse. co_issue pairs both ways from one start and keeps
  the way with fewer instructions, the second on a tie.
- **Exact:** each form is its vector op as Xenia's interpreter runs both (xenia:src/xenia/gpu/shader_interpreter.cc:
  334-401, 655-761, 824-829), on the same operands read the same way. src3's a is lane w's component and b lane x's,
  so a max or a min keeps its operands' order, which a NaN and equal zeros of two signs show; src3 has one negation, so
  x + -y is `subs` of x and y, and -x + y `subs` of -x and -y, -x - -y, which rounds as -x + y; add and mul alone
  commute for a constant to take a's place. A multiply or a max with one operand negated or absolute, a max or min with
  a constant, and operands in two registers stay vector ops. The negation of `mulsc`, `addsc` and `subsc` applies to
  both operands as Xenia's interpreter and documentation take it (ucode.h:961-962), and as the probe showed; Dusklight
  relies on it only in 19 vertex `subsc`s, `mix(1.0, inversesqrt(...), step(...))`'s y - 1.
- **Lanes apart** (`31e1bf6`, `scalar_form`, `other_lane`; src/xsc_program.c, `register_kinds`): a constant form's
  constant is read in a lane other than its temp's. A literal is read from another lane of its register holding the
  same number, as a splat's lanes do; an op on a uniform in its temp's lane stays a vector op; and the allocator leaves
  such a form's temp in its lanes, where packing had moved it onto the constant's in 20 programs, 19 of Dusklight's
  vertex shaders among them, at no program's register.
- **dp2add, not built:** a dot product with a literal 1 becomes `dp2add` exactly only where the 1 is its last lane, as
  Xenia's dp3 adds its products from x on and dp2add its addend last (shader_interpreter.cc:449-474). Dusklight's 300
  dp3s with a literal 1 hold it in lane x, the lights' (1, d, d * d) and (1, cos, cos * cos), and none in lane z; the
  rest of the corpus has 5 that could take it. Built for those, it would have shown no win, and a dp2add, reading three
  operands, can't co-issue where a dp3 can.

**The first console run,** of the first build, built clean from the M48 pair (local/m48/step5_build.sh), Dusklight's
pack rebuilt with its xost:
- `probe_shader_cost`: Dusklight's vertex shaders took 12,609 us, under the milestone's 12,686, and its pixel shaders
  51,557. 100 of the 103 pictures were the baseline's bit for bit; `8ecfa22174a91d18`, `b624738d17bc5ca0` and
  `be7931e3ec0bb85e` weren't, which hold `mulsc r0.w, c0.w, r1.w`, `mulsc c2.w, r1.w` and `mulsc c4.x, r1.x`.
- `probe_xsc`: 150 of 150 programs as the pack holds them, in 1.236 s.
- xsdk_test passed 462 of 465, failing the 2 controller checks with no pad and the raster test's `vk_loop_grid.frag`,
  472,000 of 472,000 pixels wrong, which holds `addsc r15.z, c13.z, r9.z`. vulkan4xenon_test passed 486 of 492 twice,
  failing `branch_data.vert`, `branch_uniform.vert` and `loop_break.frag` compiled on the console, the optimiser on.
- The programs whose pictures matched held `subsc c105.y, r0.z`, `mulsc c145.w, r0.y` and `subsc c105.y, r1.z`.

**The probe,** `probe_scalar_forms` (`456e176`; log `probe_scalar_forms.log`), runs 695 pixel shaders, each one
instruction on r0 to r63 loaded with values every lane of which is its own: 587 passed and 108 failed, 36 each of
`mulsc`'s, `addsc`'s and `subsc`'s 160, and every failure was a case whose constant's component was its temp's, x and
x or w and w. There the GPU gave the temp with itself under the one negation: r1.x * r1.x for `mulsc c0.x, r1.x`, twice
the temp for `addsc`, 0 for `subsc`. Every other case passed bit for bit as Xenia's model says: those forms over temps
r0 to r63 through each bit of the index, every lane of four constants, negated or not, alone or beside a vector op;
`adds`, `subs`, `muls`, `maxs` and `mins` over every pair of lanes; `floors`, `frcs`, `truncs` and `maxs` copies; the
`rcp` control and the vector halves. docs/HARDWARE.md, "The scalar unit's constant forms, milestone 48 step 5", has
the table. Every console failure above holds such a form as the PC compiles it.

**xost's simulators** (`31e1bf6`, xdis/simulate.py, xdis/equivalence.py) read the temp for both where the lanes match,
as the console does, which Xenia's interpreter doesn't (shader_interpreter.cc:655-671). With them the probe's cases,
which tools/gen_scalar_forms_probe.py now builds expecting it, give what the console gave, all 695.

**xost's tests:** every one passes. `tests/test_scalar_unit.py` runs `xsc/testdata/vk_scalar_unit.frag` on the
simulator, the optimiser off and on, at inputs with NaN and -0 in a max's and a min's first operands and at ordinary
ones: every export must be the same bit for bit, a NaN as NaN; with the optimiser on, the program must hold each form
above, each beside a dot product, 20 scalar ops in all, every one co-issued, keep a multiply of two registers, one with
a negated operand, a max with a constant, an absolute value's max and add and a dp3, and take 38 ALU instructions, 157
off. mulsc, addsc and subsc built by hand on r45, r18 and r33, whose indices use every bit, must give what their
constant and temp make. Against `4bf5d02`'s compiler it failed on its counts: 58 ALU instructions, no scalar op.
Broken on purpose it fails seven ways: with a max's and a min's operands swapped or the temp's index without its bit 1
or bit 0 on its exports, without its bits 2 to 5 on the ops built by hand, and with the negation dropped, a multiply
with one operand negated taken or operands in two registers taken on its scalar ops. `test_optimize.py`'s and
`test_fetch_fills.py`'s counts drop where a one-lane copy or op now co-issues, each checked by hand;
`test_dynamic_index.py`'s simulator runs the scalar forms. Since `31e1bf6` the test checks the rule too:
`vk_scalar_unit.frag`'s literals read in lanes apart from their temps', a multiply of a uniform's x and a temp's x kept
in the vector unit beside one of the temp's y that takes `mulsc`, and every program of the corpus, xost's testdata,
Dusklight's GX shaders and vulkan4xenon's, 431 of 446 compiled, without such a form; the `subsc` built by hand on
r33.w, its constant's lane w too, gives 0. Against step 5's first compiler it failed each way: `vk_scalar_unit.frag`'s
exports differed and two of its forms had equal lanes, the uniform's case's exports differed, and the corpus held 83
such forms. `test_optimize.py`'s case of a component read before its copy takes 6 instructions, 5 at the first build,
its two adds of a uniform in the temp's lane kept.

**The counts,** `step5.txt` and `step5.json`, the baseline's command with `--baseline`; 0 shaders differ off against
on, 50 not simulated and 15 not compiled, as at step 4, with `31e1bf6`'s compiler and simulators. It ran over 446
files, step 4's and `vk_scalar_unit.frag`; the table is over the shaders the baseline compiled. Optimiser on,
baseline -> step 4 -> step 5's first build, `7dc5220` -> step 5, `31e1bf6`:

| | Corpus vertex, 135 | Corpus pixel, 290 | Dusklight vertex, 47 | Dusklight pixel, 103 | Costliest vertex, 10 | Costliest pixel, 15 |
|---|---|---|---|---|---|---|
| ALU | 4518 -> 4515 -> 4132 -> 4156 | 4225 -> 3934 -> 3778 -> 3798 | 3257 -> 3257 -> 2965 -> 2965 | 1342 -> 1086 -> 998 -> 1000 | 1473 -> 1473 -> 1313 -> 1313 | 546 -> 409 -> 368 -> 369 |
| Copies | 636 -> 636 -> 415 -> 417 | 1032 -> 1052 -> 948 -> 965 | 305 -> 305 -> 147 -> 147 | 118 -> 138 -> 94 -> 94 | 170 -> 170 -> 55 -> 55 | 37 -> 41 -> 17 -> 17 |
| Co-issued | 528 -> 527 -> 910 -> 886 | 199 -> 131 -> 287 -> 267 | 471 -> 471 -> 763 -> 763 | 159 -> 96 -> 184 -> 182 | 222 -> 222 -> 382 -> 382 | 96 -> 45 -> 86 -> 85 |
| Registers | 720 -> 720 -> 723 -> 721 | 1403 -> 1381 -> 1376 -> 1376 | 300 -> 300 -> 299 -> 299 | 466 -> 448 -> 440 -> 440 | 94 -> 94 -> 92 -> 92 | 124 -> 120 -> 113 -> 113 |
| Single-lane | 1921 -> 1921 -> 1474 -> 1520 | 578 -> 509 -> 332 -> 351 | 1623 -> 1623 -> 1289 -> 1289 | 383 -> 318 -> 200 -> 202 | 602 -> 602 -> 475 -> 475 | 186 -> 151 -> 100 -> 101 |
| of them `add` `mul` `max` `min` `floor` `frc` | 873 -> 893 -> 681 -> 705 | 402 -> 353 -> 187 -> 206 | 713 -> 713 -> 558 -> 558 | 271 -> 228 -> 116 -> 118 | 294 -> 294 -> 231 -> 231 | 128 -> 109 -> 63 -> 64 |

- **This step's own effect:** Dusklight's vertex shaders take 9.0% fewer ALU instructions than at step 4, its 10
  costliest 10.9%; its pixel shaders 7.9%, the 15 costliest 9.8%, 25.5% and 32.4% under the baseline. No shader takes
  more ALU instructions than at step 4, off or on; with the optimiser off none changes. 6 programs take a register
  more and 8 fewer, in Dusklight `f12926532ee0b4d0.vert`, 8 to 9.
- **What the rule costs,** against the first build: Dusklight's vertex shaders nothing, their 19 `subsc`s reading their
  literal in another lane; its pixel shaders 2 ALU instructions and 2 co-issued slots, `85c8b022385334bf.frag` and
  `e19f86ee747cb140.frag` each keeping a multiply of a uniform in its temp's lane; the corpus's vertex shaders 24 and
  pixel shaders 20, in vulkan4xenon's Aurora vertex shaders 1 or 2 each and its `loop_break.frag` 15, 223 as at step 4.
  No program takes a register more; two take one fewer.
- **The scalar ops in Dusklight's pack:** in its vertex shaders 144 `maxs`, every one a copy, 77 of them of a literal,
  the lights' 1s among them, 135 `muls`, every one a square, the lights' cos * cos among them, and 19 `subsc`, beside
  the 300 `rcp`s, 150 `sqrt`s and 21 `rsq`s they had; in its pixel shaders 29 `subs`, 29 `floors`, 16 `maxs`, 6
  `mulsc`, 8 before the rule, and 6 `subsc`.
  In the 10 costliest vertex shaders, a copy of a literal sits beside a `sqrt`, `rcp` or `rsq` in 112 slots, 142 at
  step 4.
- needless.py's vertex mix pairs fall 21 to 2 only because their `add` of y and -x is now a `subsc`, which it doesn't
  follow; no mix folded.
- **Left, in Dusklight:** its vertex shaders' 1289 single-lane ops are 712 dot products, which have no scalar form;
  288 maxes of a constant and a temp, the lights' max(0, x), for which the scalar unit has no constant form; 256
  multiplies of two registers' values, the lights' attenuation times its reciprocal and times n dot l among them; 14
  multiplies of one register's left without a partner; and 19 mads. Its pixel shaders' 202 are 77 mads, 74 adds and
  multiplies of a constant and a temp, 2 of them on a uniform in the temp's lane, and 11 floors left without a partner,
  and 40 others. The maxes and the multiplies
  of two registers would take the allocator placing both operands, or a 0, in one register before co_issue, which
  pack_registers does after it: the step leaves that work, up to 544 more ops in Dusklight's vertex shaders that could
  take the scalar pipe, of which a pair saves one slot.

**Containers that change,** compiled by `cec88d4` and by `31e1bf6`, per program in `step5_containers.txt`:
- **Dusklight:** 88 of its 150 programs with the optimiser on, none off; 4343 ALU instructions to 3965. 29 differ from
  the first build's, the three whose pictures changed and the 19 vertex shaders with `subsc` among them. Its pack must
  be rebuilt.
- **vulkan4xenon:** none of `tests/shaders`, `src/shaders` and `bench/shaders` with the optimiser off, as its bundles
  are built; with it on, 21 of `tests/shaders`' 80 (1296 to 1234) and `bench/shaders`' `mesh_gray.frag` (3 to 2).
- **xsdk_test's 46:** 6 change, 1689 ALU instructions to 1654: `vk_vertex_if.vert`, `vk_dynamic_interpolator.vert`,
  `vk_slow_fetch.frag` (a `mulsc`), `vk_loop_grid.frag`, `vk_vertex_loop_break.vert` and `vk_scalar_input.frag`;
  `vk_loop_if_break.frag` is as at step 4 again. `tests/xsc_shaders.h` regenerated, 4 of its programs unlike the
  first build's.

**Compile time:** compiling Dusklight's 150 programs on the PC, best of 25 runs in six alternations, took 74.9 to
78.0 ms at step 4, 72.3 to 76.9 at the first build and 72.2 to 73.4 now: the rule costs nothing measurable. The scan
going on past a pair took 8% off, and the second pairing gives half of it back. The console took 1.236 s at the first
build, within 1.42.

**What's left for the console,** built clean from the M48 pair at this step, Dusklight's pack rebuilt with this xost:
- `probe_shader_cost`: Dusklight's vertex shaders, the milestone's open target, at most 12,686 us, 12,609 at the first
  build; every pixel picture as the baseline's, bit for bit, `8ecfa22174a91d18`, `b624738d17bc5ca0` and
  `be7931e3ec0bb85e` among them.
- `probe_xsc`: 150 of 150 programs as the pack holds them, within 1.42 s.
- xsdk_test, its compiler check taking the 6 changed programs and its raster test `vk_loop_grid.frag`, 0 pixels wrong;
  vulkan4xenon_test twice, 492 of 492, `branch_data.vert`, `branch_uniform.vert` and `loop_break.frag` compiled on the
  console among them, its bundles as they are.
- `probe_scalar_forms` again, its cases now expecting the temp with itself where the lanes match: 695 of 695.
- Dusklight's title scene, whose lighting alone shows the 19 vertex `subsc`s' negation and their literals' lanes, as
  the probe checks no vertex results.
