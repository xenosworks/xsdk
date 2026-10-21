# Milestone 55

The three probe logs here are the user's console results from steps 1 and 2. `pcsx360_*` are the user's supporting
text logs. None is a result from the new SDK wrappers. See docs/HARDWARE.md for the measured facts and limits.

## SDK checks

Build in WSL Debian from this worktree:

```sh
make clean && make -j32
make m55_host_check m55_flush_check
sh tests/breaks/m55/build.sh
```

- `build/m55_check.xex` runs the new checks without the GPU suite. Launch this first and send `m55_check.log`.
  As with the probes, `net_log.txt` can stream it to `tools/net_log.py`; `dbgprint.txt` opts into Watson output.
- `build/xsdk_test.xex` includes those same checks plus the existing suite. Its normal network and input setup still
  applies. Console acceptance needs this run too.
- `build/m55-breaks/m55_<case>.xex` runs one intentional defect. The exact expected failing checks are listed in
  [cases.tsv](../../tests/breaks/m55/cases.tsv). These are negative builds: never ship them. The script compiles
  private source copies in parallel and never patches the working tree. Optional case names select a subset.
- `jit_noflush` must fail the **host** compiled-sequence check, recorded in `build/m55-breaks/noflush-check.log`.
  Its console return checks may still pass: the probe already saw an unflushed rewrite. `jit_first` and
  `jit_rewrite` provide deterministic console return-value failures.

Host checks use mocked kernel calls: partial unmap, protection/validation, kernel error recovery, 20,000 concurrent
semaphore posts/takes, deadline and overflow handling, heap trim/recommit and failed-trim recovery. They establish
SDK bookkeeping, not the console ABI, cache behaviour, scheduling, protection or free-page counts.

## Remaining acceptance

- Run the clean `m55_check` and `xsdk_test`, then the intended negative builds and retain their logs here. No such
  console runs were performed during implementation.
- Run vulkan4xenon_test twice, vkbench unchanged, SDL-xenon's sdl_check, and Dusklight's title scene once. Compare
  Dusklight performance with its previous result now that requested priorities reach the kernel. Those projects
  were not modified by this work.
- The user retained GCC's builtin/libgcc clear-cache patch and toolchain rebuild. Until that is done, generated
  code must call `xsdk_code_flush` explicitly.
- `NtQueryVirtualMemory` remains a failed raw probe result. Mappings use owned page bookkeeping and the protection
  checks use `MmQueryAddressProtect`. A correct raw query ABI still needs its own hardware investigation.

The milestone is not marked console-complete until its acceptance runs pass.

## Local validation completed (2026-10-20)

- A fresh `make clean && make -j32` succeeded with zero compiler warnings/errors after the final source changes.
- `make m55_host_check m55_flush_check` passed all four checks (mappings, concurrent semaphores, heap trim, assembly).
- All 43 negative titles built without compiler diagnostics from isolated source copies. The compiled-sequence
  check rejected `jit_noflush` as intended. These builds are not claims that the console has run the negative cases.
- The new public headers compiled together as C++23. ELF inspection confirmed the quick title's 64 KB-aligned
  64 KB `.jit` section has alloc/write/execute flags and its pool boundary symbols differ by 64 KB.
- Source hashes confirmed that building negative cases did not change their implementation inputs.

Build logs are local, ignored files in `local/m55/`; binaries and each mutation's build log are in `build/`.
