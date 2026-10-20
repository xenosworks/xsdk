# Milestone 52: compiled CPU code

docs/scope/52-compiled-cpu-code.md; docs/HARDWARE.md, "Counted loops end in bdnz, milestone 52". Built in the M52
worktree pair with the old toolchain (`~/xsdk-gcc`) and the new (`~/xsdk-gcc-m52`, gcc-xsdk.patch's
`rs6000_preferred_doloop_mode` hunk), run on the console through the console agent on 2026-10-19.

## Step 1: the probes

- **`probe_ctr.log`:** CTR set to 0x3, 0x0000000100000003 and 0xFFFFFFFF00000003, `bdnz` passed 3 times each, the high
  word left as it was; 0xFFFFFFFF + 1 carried 1; MSR 0x9030, SF 0, HV 0, PR 0. A title runs in 32-bit mode and `bdnz`
  tests CTR's low word, so a counter GCC moves to CTR from a 32-bit value needs no clean high word.
- **`probe_atomics_new.log`**, one thread, best of 5 of 10^6 (`probe_atomics_old.log` within 4% but release, 76.6 ns):

  | | ns | CPU clocks |
  |---|---|---|
  | a plain increment of memory | 16.1 | 51 |
  | `__atomic_fetch_add`, relaxed | 54.7 | 175 |
  | acquire | 69.6 | 222 |
  | release | 69.4 | 222 |
  | acq_rel | 84.7 | 270 |
  | seq_cst | 124.5 | 398 |
  | `lwsync` alone | 11.6 | 37 |
  | `sync` alone | 53.1 | 169 |
  | `isync` alone | 12.1 | 39 |

  Two threads on one counter: on two cores 103 to 119 ns an increment relaxed and 139 to 150 seq_cst; on one core 146
  to 173 relaxed and 285 seq_cst; every count exact.

## Step 2: the fix

The hunk returns the pointer mode, so a counted loop's counter is 32-bit and `doloop_end` takes it. `sizes.txt`, old
then new: libxsdk.a's text 510,544 to 509,416 bytes, `bdnz` 213 to 321 and loops ending in `cmpdi`/`bne` 16 to 1;
xsdk_test.elf 3,121,533 to 3,118,897, `bdnz` 451 to 704, `cmpdi` loops 234 to 6; vulkan4xenon_test.elf and vkbench.elf
alike. tools/xenia_scan.py's counts are unchanged. Dusklight (`dusk_sizes.txt`): `bdnz` 3,837 to 7,393, `cmpdi` loops
4,724 to 1,147, of which SDL3, prebuilt with the old toolchain, holds 1,111. The Makefile's check
(tests/doloop_check.c) fails the old toolchain and passes the new.

## Step 3: everything on the console

- **xsdk_test** (`xsdk_test.log`, `xsdk_test_old.log`): 463 passed, 2 failed both ways, the controller's checks.
- **vulkan4xenon_test** (`vulkan4xenon_test.log`, `_old.log`): 492 of 492 both ways.
- **vkbench** (`vkbench.log`, `_old.log`): 17 of 17 both ways, its draw and submit times within 2% of each other, but
  vulkan4xenon's `uniforms_1000`, 669 against 621 us in that pair of runs and 669 again in `vkbench_new.log`.
- **probe_unroll's loops at -O2** (`probe_unroll_old.log`, `_new.log`), clocks a unit:

  | Loop | Old | New |
  |---|---|---|
  | 8 multiply-add chains | 1.668 | 1.567 (−6.1%) |
  | multiply-add over arrays | 13.774 | 13.662 (−0.8%) |
  | 4x4 matrix times vectors | 46.812 | 48.300 (+3.2%) |
  | word copy | 7.518 | 7.167 (−4.7%) |
  | FNV-1a over bytes | 16.027 | 16.036 |
  | MurmurHash3 finalizer | 41.694 | 41.360 (−0.8%) |

- **Dusklight's diagnostics build** (`dusk_diag_old.log`, `dusk_diag_new.log`; milestone 35's diagnostics, ported),
  means over phases 0 to 8: a frame 35.0 and 35.1 ms (the GPU holds it), the FIFO worker 11.88 and 11.81 ms, the
  render worker 3.69 and 3.71, the replay 0.86 and 0.84, the GPU 10.06 and 10.03. Its CPU work doesn't change: its hot
  loops aren't counted loops of this kind. `dusk_new.log`, the plain build, ran its title scene at 29.8 to 30 fps.

## Step 4: unrolling measured again

libxsdk and libvulkan4xenon alone built with `-funroll-loops`, against the new toolchain's -O2:
- **Dusklight** (`dusk_diag_new_unrollsdk.log`): the FIFO worker 11.80 ms against 11.81, the render worker 3.60
  against 3.71, the replay 0.87 against 0.84; the heap's peak the same.
- **vkbench** (`vkbench_new_unrollsdk.log` against `vkbench_new.log`): xsdk's scenes 0 to 4.5% less CPU, vulkan4xenon's
  0 to 5% (`states_1000` 1,320 to 1,252 us, `draws_4000` 3,566 to 3,472).
- **The cost:** libxsdk's text 509,892 to 637,076 bytes, and tools/xenia_scan.py's count in it 2 to 44, `rldic` in
  xost's compiler, which Xenia skips.

So unrolling stays off: a few percent in vkbench, nothing in Dusklight, against a quarter more code and Xenia runs
made unreliable through the compiler.
