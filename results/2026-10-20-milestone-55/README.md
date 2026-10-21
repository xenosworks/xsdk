# Milestone 55

The three probe logs here are xsdk console results from steps 1 and 2.
[probe_exec_spike.log](probe_exec_spike.log) establishes the `.jit` method. None tests the new SDK wrappers. See docs/HARDWARE.md for the measured facts and limits.

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
  Its console return checks may still pass; logging in the probe did not exclude incidental cache eviction. `jit_first` and
  `jit_rewrite` provide deterministic console return-value failures.
- `jit_range_end` drops the last-byte subtraction. It must fail the host trace at the aligned 128-byte case;
  see `build/m55-breaks/range-end-check.log`. The normal trace also checks zero bytes, 129 bytes, an unaligned
  crossing, a three-line span and the last address. It substitutes only the cache instructions in `src/jit.c`,
  preserving its actual C range arithmetic and loops. This tests addresses and ordering, not hardware coherence.
- `pages_mmap_execute` and `pages_mprotect_execute` each break one executable-memory refusal independently.
  `jit_crossing` and `jit_129` independently break the two added console range checks.

Host checks use mocked kernel calls: partial unmap, protection/validation, kernel error recovery, 20,000 concurrent
semaphore posts/takes, deadline and overflow handling, heap trim/recommit and failed-trim recovery. They establish
SDK bookkeeping, not the console ABI, cache behaviour, scheduling, protection or free-page counts.

## Remaining acceptance

- Run the clean `m55_check` and `xsdk_test`, then the intended negative builds and retain their logs here. No such
  console runs were performed during implementation.
- Run vulkan4xenon_test twice, vkbench unchanged, SDL-xenon's sdl_check, and Dusklight's title scene once. Compare
  Dusklight performance with its previous result now that requested priorities reach the kernel. Those projects
  were not modified by this work.
- GCC's builtin/libgcc clear-cache patch and rebuild are tracked in [TODO.md](../../TODO.md). Until then,
  generated code must call `xsdk_code_flush` explicitly.
- `NtQueryVirtualMemory` remains a failed raw probe result. Mappings use owned page bookkeeping and the protection
  checks use `MmQueryAddressProtect`. A correct raw query ABI still needs its own hardware investigation.

The milestone is not marked console-complete until its acceptance runs pass.

## Initial local validation (2026-10-20, before the C-flush review)

- A fresh `make clean && make -j32` succeeded with zero compiler warnings/errors after the final source changes.
- `make m55_host_check m55_flush_check` passed all four checks (mappings, concurrent semaphores, heap trim, assembly).
- All 43 negative titles built without compiler diagnostics from isolated source copies. The compiled-sequence
  check rejected `jit_noflush` as intended. These builds are not claims that the console has run the negative cases.
- The new public headers compiled together as C++23. ELF inspection confirmed the quick title's 64 KB-aligned
  64 KB `.jit` section has alloc/write/execute flags and its pool boundary symbols differ by 64 KB.
- Source hashes confirmed that building negative cases did not change their implementation inputs.

Build logs are local, ignored files in `local/m55/`; binaries and each mutation's build log are in `build/`.

## Review validation (2026-10-20)

- M55 was rebased onto local main. The two prohibited third-party evidence files are absent from the rewritten
  branch's reachable history; current citations use xsdk's own spike log. Old local Git objects/reflogs were not
  purged from the shared repository.
- `src/jit.c` now contains the flush; the assembly helper is removed. The agreed `xsdk_code_flush` API name is kept.
- The clean full rebuild completed with zero warnings. All five host checks passed, including 11 exact range traces.
- All 47 negative titles built without warnings. The no-flush and missing-subtraction variants were rejected by
  their respective host checks. Console outcomes for these builds and the new range checks remain pending.
- GCC and the installed toolchain were unchanged. Build logs are `local/m55/review-*.log`.
