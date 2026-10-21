# Milestone 55

Update (2026-10-21): the [acceptance set](../2026-10-21-milestone-55-acceptance/) has four baseline failures.
M55 is not console-ready. Run the revised diagnostic baseline before repeating the negative titles.

The logs here are xsdk console results from steps 1 and 2 and the memory-query follow-up.
[probe_exec_spike.log](probe_exec_spike.log) establishes the `.jit` method;
[probe_query_memory.log](probe_query_memory.log) establishes the three-argument virtual-memory query and its
refusal of image pages. None tests the new SDK wrappers. See [HARDWARE.md](../../docs/HARDWARE.md) for the measured
facts and limits.

## SDK checks

Build in WSL Debian from this worktree:

```sh
make clean && make -j32
make m55_host_check m55_flush_check m55_heap_check
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

- The first `m55_check`, `xsdk_test` and negative runs are in the newer acceptance folder linked above.
  The revised focused baseline must pass before the affected negatives and full SDK run are accepted.
- Run vulkan4xenon_test twice, vkbench unchanged, SDL-xenon's sdl_check, and Dusklight's title scene once. Compare
  Dusklight performance with its previous result now that requested priorities reach the kernel. Those projects
  were not modified by this work.
- GCC's builtin/libgcc clear-cache patch and rebuild are tracked in [TODO.md](../../TODO.md). Until then,
  generated code must call `xsdk_code_flush` explicitly.

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

## Three-argument memory query result

[probe_query_memory.log](probe_query_memory.log) is the unchanged v1 console log, copied from
`Z:\Xbox-360-SDK\local\m55\logs\query\probe_query_memory.log`. Its SHA-256 is
`3ededd0adfc1941567c8322dcc39b797aaa738d0e329b09d84bd3066e5853e50`.

All six allocation queries passed: committed read-write, interior address, read-only first page, writable
neighbour, decommitted/reserved page and released/free range. The seven-word results had intact guards and
matched the controlled regions and `MmQueryAddressProtect`. Title code and `.jit` returned C000000D, left the
output untouched and kept the guards intact; `MmQueryAddressProtect` returned 00000020 for both. The log's
**20 passed, 2 failed** comes from v1 expecting successful image queries. The splash passed.

The public declaration and ordinal 0x0EE import now take `(address, information, 0)` for regions from
`NtAllocateVirtualMemory`, including `mmap`'s backing allocations. Use `MmQueryAddressProtect` for image
protection. Only zero is established for the third argument; its meaning and nonzero values remain unverified.
The log tests the raw allocation API, not the `mmap` wrapper. Xenia cannot validate the third argument.

`build/probe_query_memory.xex` v2 uses the public import, keeps the six allocation checks and expects
`STATUS_INVALID_PARAMETER` for both image queries. It retains output guards and the splash, and does not
interpret an error result as valid memory information. Guard corruption stops further calls. The free-range
query still runs before logging after release. Launch with no configuration; `net_log.txt` streams the log
and `dbgprint.txt` enables Watson output. The retained v1 log is not a console run of this revised probe.

The C host tests and focused title are explicit M55 exceptions in CODE_GUIDELINES.md. `m55_check` and its negative
variants now show the splash. The host semaphore test fills its mock handle table, checks `sem_init` returns
`ENOSPC` without changing the failed object, then frees the resources and retries successfully. This follows
[POSIX sem_init](https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/functions/sem_init.html).

Before the console query result, a clean full rebuild had zero warnings; all six host checks passed, and restoring
`ENOMEM` failed the new resource-exhaustion assertion. All 47 negative titles rebuilt without diagnostics. Object
inspection confirms the candidate call sets r5 to zero; archive inspection confirms the old query import is absent.
Logs are `local/m55/followup-*.log`. The later v1 console query and its splash are recorded above;
`m55_check`'s splash and the revised v2 query probe have not yet been run on the console.

## Restored query validation (2026-10-20)

- A clean full rebuild completed with zero compiler/linker warnings or errors; all six M55 host checks passed.
- Linked PowerPC inspection confirms ordinal 0x0EE and r5=0 at the public `NtQueryVirtualMemory` call.
- The copied console log matches its source byte for byte and has the SHA-256 recorded above.
- These build and host checks do not establish a new console result for v2. Logs and disassembly are in
  `local/m55/query-restored-*`.

The later [query v2 log](../2026-10-21-milestone-55-acceptance/probe_query_memory_v2.log) passed all 22 checks.
