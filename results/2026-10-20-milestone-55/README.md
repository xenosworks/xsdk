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

- `build/m55_check.xex` shows the splash, then runs the new checks without the GPU suite. Launch this first and
  send `m55_check.log`.
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
- `NtQueryVirtualMemory` is withheld from the public SDK pending the three-argument probe below. Mappings use owned
  page bookkeeping and protection checks use `MmQueryAddressProtect`.

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

## Three-argument memory query probe

Launch `build/probe_query_memory.xex` with no configuration file and retain `probe_query_memory.log`.
`net_log.txt` streams the log as usual; `dbgprint.txt` enables Watson output. It shows the splash, resolves kernel
ordinal 0x0EE and calls the candidate `(address, information, 0)`. The last argument is deliberately zero, a
hypothesis suggested by the rejected two-argument run, not a verified length or debug-memory flag.

It logs every call and all seven output words, with guard words before and after the output. It checks a committed
allocation and an interior address, then a read-only page and its writable neighbour, a decommitted page that stays
reserved, the released range, title code and `.jit`. A successful status alone is insufficient: the returned region
must contain the address, state/allocation must match the controlled case, and committed protection must agree with
`MmQueryAddressProtect`. Guard corruption stops further calls. The free-range query runs before logging after release.
The third argument's semantics need further evidence even if zero works. Xenia's two-argument implementation cannot
validate this candidate. No new hardware result is claimed until the console log arrives.

The C host tests and focused title are explicit M55 exceptions in CODE_GUIDELINES.md. `m55_check` and its negative
variants now show the splash. The host semaphore test fills its mock handle table, checks `sem_init` returns
`ENOSPC` without changing the failed object, then frees the resources and retries successfully. This follows
[POSIX sem_init](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/functions/sem_init.html).

Follow-up local validation: a clean full rebuild had zero warnings; all six host checks passed, and restoring
`ENOMEM` failed the new resource-exhaustion assertion. All 47 negative titles rebuilt without diagnostics. Object
inspection confirms the candidate call sets r5 to zero; archive inspection confirms the old query import is absent.
Logs are `local/m55/followup-*.log`. The query probe and splash changes still require console validation.
