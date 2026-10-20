# Milestone 52: compiled CPU code

Agreed on 2026-10-19, to run beside milestone 48 and the others agreed that day.

## Why

The toolchain's GCC 15 builds xsdk, vulkan4xenon and every title, so what it emits sets what a title's CPU does
each frame. Ports that emulate or recompile are CPU-bound, so this matters most to them.

**Counted loops never end in `bdnz`.** The spec's `-mpowerpc64` makes GCC's word mode 64-bit while pointers stay
32-bit:
- `rs6000_preferred_doloop_mode` returns the word mode (gcc-15.3.0 gcc/config/rs6000/rs6000.cc:28776-28779), so
  IVOPTs builds the loop's counter 64-bit (tree-ssa-loop-ivopts.cc:5790).
- The `doloop_end` pattern then refuses it as not the pointer mode (rs6000.md:13686-13696; loop-doloop.cc:756-778),
  and the dump says "Doloop: Target unwilling to use doloop pattern!".
- A counted loop ends in `addi`, `cmpdi` and `bne` instead of `bdnz`: two instructions more an iteration.
- Built with `-mno-powerpc64` or `-fno-ivopts`, the same loops get `bdnz`.
- A scan of the binaries finds about 1,000 such loops in xsdk_test and about 10,000 in Dusklight. The scan is a
  heuristic.

In local/hwbench's loops the two instructions are most of what a loop's overhead costs:
- its independent integer loop takes 19 instructions an iteration where 17 would do;
- two threads on one core run dependent chains at the rate their instruction count sets;
- its independent float loop takes 13.3 clocks an iteration for 8 multiply-adds against a floor of 10.

**Atomics cost what their memory order asks.** GCC 15 emits:

| Order | Code |
|---|---|
| relaxed | a plain `lwarx`/`stwcx.` loop |
| acquire | the loop + `isync` |
| release | `lwsync` + the loop |
| seq_cst | `sync` + the loop + `isync` |

C++'s `operator++` and `__sync_*` are seq_cst. xsdk's own code already passes explicit orders (src/pthread.c,
src/file.c, src/socket.c). What each order costs on the console isn't measured, nor `lwsync` against `sync`.

## What it takes

1. **Console probes, then the measurement.**
   - `bdnz` tests CTR's full 64 bits when MSR[SF] is set. A probe sets CTR to 0xFFFFFFFF00000003 and counts the
     passes, so the fix's one risk is settled before it lands. Xenia tests only the low word
     (xenia:src/xenia/cpu/ppc/ppc_emit_control.cc:193, 299), and the retail kernel's mode for a title is unproven.
   - hwbench's CPU loops and atomic increments at each memory order, with `lwsync` against `sync`, timed on the console.
   - A real workload's CPU time: Dusklight's frame diagnostics and vkbench's recording.
2. **The fix in gcc-xsdk.patch:**
   - `rs6000_preferred_doloop_mode` returns the pointer mode when the target isn't 64-bit;
   - the toolchain is rebuilt into a prefix of its own (`~/xsdk-gcc-m52`) until the milestone is done;
   - a Makefile check compiles a known counted loop and fails without `bdnz`.

   Code gets smaller, not larger. If step 1 shows CTR's high word counts, the fix also clears it in the loop's
   set-up, or the milestone comes back to the user.
3. **Everything rebuilt clean against it and run on the console:**
   - xsdk_test;
   - vulkan4xenon_test twice;
   - vkbench;
   - hwbench;
   - Dusklight's title scene with its diagnostics;
   - text sizes and tools/xenia_scan.py's counts against the old toolchain, recorded.
4. **Unrolling measured again.** Milestone 35 left `-funroll-loops` off: Dusklight's render worker's own work went
   from 2.0 to 4.4 ms through xxhash's XXH3, and its image grew 3 MB (docs/HARDWARE.md, "Loop unrolling, milestone
   35"). Part of what unrolling bought was spreading the loop overhead that step 2
   removes. `-funroll-loops` for libxsdk and vulkan4xenon alone, against `#pragma GCC unroll` on hot loops, is kept
   only where Dusklight doesn't lose.
5. **Memory orders documented,** with step 1's costs, in the guide milestone 41 writes.

Left out:
- **VMX128** (128 vector registers and their extra instructions). binutils 2.47 has none. libxenon's
  binutils-2.28-vmx128.diff, 75 mnemonics, would port to it, but GCC already uses VMX's 32 registers, and no workload
  yet is short of them. A milestone of its own when one is: the assembler first, with Xenia's `instr_*128.s` cases as
  its tests.
- **OpenMP** (`--disable-libgomp`) and **a vector maths library,** until a port uses them.
- **`-fno-gcse`** in the spec, which works around RTL PRE miscompiling 32-bit code with `-mpowerpc64`
  (tools/toolchain/build.sh). Its cost isn't measured; not changed here.

## Done when

- Counted loops in xsdk, vulkan4xenon and Dusklight end in `bdnz`, and the Makefile check holds.
- hwbench's independent integer loop takes 17 instructions an iteration. Its times and the atomics' are recorded on
  the console, before and after.
- Every title passes on the console as before, built clean with the new toolchain. Dusklight's CPU frame is no
  slower, and the gains are recorded.
- The unrolling choice is recorded with its measurement.
