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

Step 1's two baseline runs of `probe_shader_cost` on the console aren't in this folder yet.
