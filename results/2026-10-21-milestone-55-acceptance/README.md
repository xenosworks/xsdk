# M55 acceptance failures (2026-10-21)

M55 is **not console-ready**. These 52 unchanged logs were copied from
`Z:\Xbox-360-SDK\local\m55\logs_set`; [SHA256SUMS](SHA256SUMS) records their hashes.
They contain the focused baseline, the full SDK run, all 47 negative titles, two Vulkan runs and vkbench.
The separate [query v2 log](probe_query_memory_v2.log), from `local/m55/logs/query2/`, passed 22 checks with none failed.

## Baseline failures

[m55_check.log](logs_set/m55_check/m55_check.log) has 51 passes and four failures.
[xsdk_test.log](logs_set/xsdk_test/xsdk_test.log) has the same four M55 failures, plus missing network configuration
and controller availability failures elsewhere in the suite.

| Check | Evidence | Change and remaining question |
|---|---|---|
| Periodic timer | Both baselines logged 17 firings. Across the 48 runs without the 20 ms period mutation: 16 once, 17 nineteen times, 18 twenty-eight times. The one-shot baselines took 5.67 and 5.99 ms. | Measure 20 firings and allow the existing 2 ms timing budget per period, for 60–140 ms total. Log each run's elapsed time, minimum/maximum interval and status. The 20 ms mutation still exceeds that budget. Console rerun pending. |
| Mixed wait-all | Failed in all 49 M55 runs without logging its status. | Keep the success requirement. Log each creation status/handle, combined wait status/elapsed time, and individual zero-time waits on failure. The kernel's refusal or timeout is not yet identified. |
| All priorities | Failed in all 49 M55 runs without showing a level or return value. | Keep exact read-back checks. Capture every requested level, setter/query return, actual priority and policy; restore and verify the original priority before printing the sweep. The failing levels and reason remain unknown. |
| Heap-top return | Free pages stayed 121074 in the focused baseline and 118946 in the full run. | The old compiled `test_pages.o` had neither a `malloc` nor `free` relocation: GCC removed the unused allocation. Write/read one volatile word on each of 8192 pages and log heap breaks and free pages before allocation, before free and after free. Keep the full 32 MB return requirement. Actual page return still needs a console run. |

The heap failure was a test defect; its old page count cannot establish that kernel decommit failed. No SDK priority
or wait semantics are changed without the missing observations. The new baseline prints `M55 acceptance diagnostics v2`.

## Negative tests

The old baseline already failed mixed-wait and heap-trim checks. Their negative runs add no distinguishing failure:
[mixed wait](logs_set/m55_mixed_wait/m55_mixed_wait.log), [heap trim](logs_set/m55_heap_trim/m55_heap_trim.log).
The [20 ms timer mutation](logs_set/m55_timer_period/m55_timer_period.log) produced only six firings, but the ordinary
baseline also failed its old count check. Rerun these after the baseline passes. The priority mutation additionally
failed the share and previous-priority checks; the all-level check still needs a passing baseline.

The no-flush and cache-range-end mutations also add no console failure, as documented: their required failures
are host checks. Every other negative log adds a failure beyond the baseline; this does not remove the baseline
blockers or establish every case independently. Expected checks remain in [cases.tsv](../../tests/breaks/m55/cases.tsv).

The updated heap-trim mutation disables decommit for every `sbrk` increment. Disabling it only for negative
increments would let the new diagnostic `sbrk(0)` perform the delayed decommit and hide the intended defect.

## Next console run

Run the rebuilt `build/m55_check.xex` first and retain its complete log, including the priority sweep and mixed-wait
statuses. Do not rerun the 47 negative titles yet. A second focused run and the full SDK run follow once the baseline
issues are understood and fixed. The supplied Vulkan and vkbench logs are preserved; no comparative performance
claim or completed port acceptance is made here.

## Local validation

`make m55_host_check m55_flush_check m55_heap_check` runs bookkeeping, cache and compiled heap-call checks.
The new heap-call check rejects the saved original object and accepts the revised one; this only establishes that
the allocation remains in compiled code.

- A clean full rebuild completed with zero compiler/linker warnings or errors.
- All seven host/build checks passed. The saved original heap test object failed the new compiled-call check.
- All 47 negative titles rebuilt without diagnostics. The existing host heap check also rejected the revised
  mutation that disables all decommit; the no-flush and cache-range-end host negatives failed as intended.
- The rebuilt `m55_check.xex` SHA-256 is
  `6bfa2c29a965be5017e3d2c518cc1e150e70d633e90a3a87f47a9762310ac408`.
- Build and check logs are in `local/m55/acceptance-*.log`. The revised titles have not run on the console;
  successful builds and host checks do not clear the baseline failures.
