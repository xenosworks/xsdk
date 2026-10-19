# Milestone 48: xost's code quality, second part

Agreed on 2026-10-19.

## Why

Milestone 45 took Dusklight's shaders 19% off the GPU, and the rest of what they cost is still mostly xost's code. On
2026-10-19 (main b150d87), `probe_shader_cost`'s way of timing gave Dusklight's 103 pixel shaders about 63.0 ms for a
draw of each over 1280x720, and its 47 vertex shaders about 13.4 ms for a draw of each over 65,535 vertices. A pixel
shader's time is the fill floor, 267.4 us at 1280x720, or its ALU instructions, whichever is longer. 40 of the 103 sit
at the floor; past it, every instruction costs. In the 15 pixel shaders and 10 vertex shaders that cost the most,
xost's code holds work the arithmetic doesn't need. The counts below are a reading of those 25; step 1 counts them
exactly over the corpus.

- **Identities with a literal 0 or 1, about 64 slots in the 15 pixel shaders.**
  - `mix` is lowered as `add(y, -x)` then `mad` whatever its operands (src/xsc_lower_glsl.c:183-192), and Dusklight's
    GX shaders mix with 0 and 1 often.
  - `xsdk_xsc_binop` folds 0 and 1 only when they are direct operands (src/xsc_lower.c:978-1021).
  - A `mad` adding 0 isn't a multiply, so `try_fuse` can't fuse it with the add after it (src/xsc_optimize.c:809).
- **Division a lane at a time.** `fdiv` takes an `rcp` and a one-lane `mul` for every lane, even for a constant
  divisor or a vector divided by one scalar (src/xsc_lower.c:1134-1146).
- **Clamps and wraps that can't change their value,** about 32 slots in the 15 pixel shaders: a ±4 clamp or a wrap at
  255/256 of a product of saturated values. xost has no analysis of a value's range.
- **Single-lane math in the vector unit.** Every ALU slot has a vector and a scalar operation
  (xenia:src/xenia/gpu/ucode.h:1854-1878), but xost emits only `rcp`, `sqrt`, `rsq` and `exp` as scalar operations.
  `co_issue` pairs only operations that are scalar already (`scalar_only`, src/xsc_optimize.c:884-890).
  - The 15 pixel shaders hold 184 single-lane vector operations, each a slot of its own.
  - In the 10 vertex shaders, 142 slots pair a `sqrt` or `rcp` with a copy of a literal.
- **Copies in programs that read a constant at a run-time index.** The optimiser's first round skips them whole
  (src/xsc_optimize.c:1486-1496). These are Dusklight's three skinned vertex shaders, which copy each row they read
  before using it.
- **The rest, smaller:**
  - alpha computed apart from its colour's vector;
  - fog coordinates copied into place;
  - one indirect texture matrix built twice, the second copy missed by common subexpression elimination.

## What it takes

Each step is built, verified as below, then the next begins, as in milestone 45.

1. **Measurement.** `tools/check_optimize.py` counts each kind above, per shader and in total: identities, lanes
   divided apart, single-lane vector operations, clamps a range makes useless, and copies by kind. The baseline for
   Dusklight's GX shaders, xost's testdata and vulkan4xenon's shaders goes in `results/`, with two baseline runs of
   `probe_shader_cost` on the console.
2. **Identities folded.** `mix` with a constant 0 or 1 operand is lowered to what remains. `x - 0`, `0 - x`, `x * 1`
   and a `mad` adding 0 are folded wherever they arise, so `try_fuse` sees the multiplies.
3. **Division.**
   - By a constant: a multiply by the reciprocal the compiler works out.
   - By one scalar: one `rcp` and one vector `mul`.
4. **Ranges.** A pass works out each value's range from saturates, clamps, constants and inputs whose format bounds
   them, and drops a `min`, `max`, clamp or wrap that can't change its value.
5. **The scalar unit.** Single-lane `add`, `mul`, `max`, `min`, `floor`, `frc` and the like become scalar operations,
   using the forms that take a constant (`mulsc`, `addsc`, `subsc`), so `co_issue` can pair them with vector
   operations. A dot product with a literal 1 lane becomes `dp2add`. `co_issue` keeps scanning after a pair rather than
   starting over, so compiling stays within its time.
6. **Programs that index constants, through the whole optimiser.** The first round's passes model `a0`: the operation
   that sets it, and the reads that use it. They keep milestone 45's rules: `a0` loaded again after an alloc, and arms
   that use it as plain execs with predicated instructions (docs/HARDWARE.md, "xost's code quality, milestone 45").
7. **The rest:** alpha packed into its colour's vector; common subexpressions across an indirect texture matrix; fog
   coordinates computed in place.

Left out:
- **Writing only an interpolator's declared lanes.** One vertex shader isn't held by its ALU and exports 36 lanes, but
  other shaders with extra lanes run no slower, so that cause is unproven. It would need a probe of its own.
- **Scheduling for latency beyond co-issue.**

## Verification, without the user

As in milestone 45:
- **On the PC, every step:**
  - xost's tests;
  - `check_optimize.py` on every shader of the corpus, the step on against off, with exports equal on random inputs;
  - containers the console has passed come out byte for byte the same unless the step targets them;
  - the counts against the baseline.
- **Exact results.** Steps 2, 4, 5, 6 and 7 keep every result bit for bit, but for a sign of zero. Folding `mix`'s
  weight of 1 gives `y` exactly, where `(y - x) * 1 + x` can round. Step 3's reciprocal of a constant is worked out in
  doubles, then rounded, and the hardware's `rcp` it replaces is no more exact. The equivalence check takes each
  step's known difference and nothing more.
- **On the console, every step,** through the console agent:
  - xsdk_test, with its compiler check;
  - vulkan4xenon_test, twice, with the shaders the step's xost optimises;
  - `probe_shader_cost`, its pictures compared with the baseline's: the same for bit-exact steps, and within one step
    of a channel where a step changes rounding;
  - Dusklight's title scene, as a smoke run;
  - break sets, each failing the checks it names.

## Done when

- `probe_shader_cost` times Dusklight's pixel shaders at least 12% under step 1's baseline and its vertex shaders at
  least 5% under it, with every picture as the verification asks.
- Compiling Dusklight's 150 programs on the console takes at most 1.42 s.
- xsdk_test and vulkan4xenon_test pass with nothing hung.
