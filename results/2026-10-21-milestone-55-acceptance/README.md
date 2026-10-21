# M55 acceptance results (2026-10-21)

M55 acceptance remains **incomplete**. Diagnostics v2 has 59 passes and one incorrect priority expectation;
see the follow-up below. The original 52 unchanged logs were copied from
`Z:\Xbox-360-SDK\local\m55\logs_set`; [SHA256SUMS](SHA256SUMS) records their hashes.
They contain the focused baseline, the full SDK run, all 47 negative titles, two Vulkan runs and vkbench.
The separate [query v2 log](probe_query_memory_v2.log), from `local/m55/logs/query2/`, passed 22 checks with none failed.

## Original baseline failures

[m55_check.log](logs_set/m55_check/m55_check.log) has 51 passes and four failures.
[xsdk_test.log](logs_set/xsdk_test/xsdk_test.log) has the same four M55 failures, plus missing network configuration
and controller availability failures elsewhere in the suite.

This table records the analysis before diagnostics v2; the follow-up below supersedes its pending questions.

| Check | Evidence | Action prepared for diagnostics v2 |
|---|---|---|
| Periodic timer | Both baselines logged 17 firings. Across the 48 runs without the 20 ms period mutation: 16 once, 17 nineteen times, 18 twenty-eight times. The one-shot baselines took 5.67 and 5.99 ms. | Measure 20 firings and allow the existing 2 ms timing budget per period, for 60–140 ms total. Log each run's elapsed time, minimum/maximum interval and status. The 20 ms mutation still exceeds that budget. Console rerun pending. |
| Mixed wait-all | Failed in all 49 M55 runs without logging its status. | Keep the success requirement. Log each creation status/handle, combined wait status/elapsed time, and individual zero-time waits on failure. The kernel's refusal or timeout is not yet identified. |
| All priorities | Failed in all 49 M55 runs without showing a level or return value. | Keep exact read-back checks. Capture every requested level, setter/query return, actual priority and policy; restore and verify the original priority before printing the sweep. The failing levels and reason remain unknown. |
| Heap-top return | Free pages stayed 121074 in the focused baseline and 118946 in the full run. | The old compiled `test_pages.o` had neither a `malloc` nor `free` relocation: GCC removed the unused allocation. Write/read one volatile word on each of 8192 pages and log heap breaks and free pages before allocation, before free and after free. Keep the full 32 MB return requirement. Actual page return still needs a console run. |

The heap failure was a test defect; its old page count cannot establish that kernel decommit failed. No SDK priority
or wait semantics are changed without the missing observations. That baseline printed `M55 acceptance diagnostics v2`.

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

## Diagnostics v2

[m55_check_diagnostics_v2.log](m55_check_diagnostics_v2.log) is copied byte-for-byte from
`Z:\Xbox-360-SDK\local\m55\logs_check2\m55_check.log`: **59 passed, one failed**.

- The revised periodic check passed: 20 firings in 118.561 ms, intervals 4.999–6.000 ms.
- The heap check passed: 121074 → 112874 → 121074 free pages, returning 8,200 pages after freeing the touched buffer.
- All 33 priority setters/queries succeeded. -16 and +16 read back unchanged, -15..-4 read back -3,
  -3..4 read back unchanged, and 5..15 read back 4. Restoration to 0 succeeded. The old expectation was wrong;
  diagnostics v3 checks this exact mapping, while SDK setters/queries continue to return the kernel's results.
- Mixed wait-all returned success in 2.646 microseconds. Logging may have let the worker finish before the wait;
  this does not establish why the earlier runs failed. V3 explicitly waits for completion in the ready control,
  and adds a target gated on hardware thread 2 with a waiter on hardware thread 4. The gate stays closed while
  the main thread waits up to 20 ms for the waiter; both waiter and target must remain unfinished. It then opens
  the gate and checks the combined wait result, semaphore consumption and mutant release by the owning waiter.
  There is no logging during that interval. The ready signal precedes the kernel wait; the test does not instrument
  the kernel's internal wait queue.

The new `mixed_pending` negative uses wait-any, which must finish before the gate opens and fail the pending check.
There are now 48 negative titles; the retained 47 logs above remain historical evidence, not results for v3.

## Next console run

Run the rebuilt **diagnostics v3** `build/m55_check.xex` first and retain the complete log. Its priority lines now
include expected values, and the pending mixed-wait lines include readiness, gate, wait, release and token statuses.
Do not rerun the negative titles yet. A repeat focused run, the full SDK run and affected negatives follow once this
baseline passes. The supplied Vulkan and vkbench logs are preserved; no comparative performance claim or completed
port acceptance is made here.

## Diagnostics v2 local validation (historical)

`make m55_host_check m55_flush_check m55_heap_check` runs bookkeeping, cache and compiled heap-call checks.
The new heap-call check rejects the saved original object and accepts the revised one; this only establishes that
the allocation remains in compiled code.

- A clean full rebuild completed with zero compiler/linker warnings or errors.
- All seven host/build checks passed. The saved original heap test object failed the new compiled-call check.
- All 47 negative titles rebuilt without diagnostics. The existing host heap check also rejected the revised
  mutation that disables all decommit; the no-flush and cache-range-end host negatives failed as intended.
- The rebuilt `m55_check.xex` SHA-256 is
  `6bfa2c29a965be5017e3d2c518cc1e150e70d633e90a3a87f47a9762310ac408`.
- Build and check logs are in `local/m55/acceptance-*.log`. The v2 hardware result is recorded above;
  these build checks alone did not clear its baseline failures.

## Diagnostics v3 local validation

- Clean full rebuild: zero compiler/linker warnings or errors.
- All seven host/build checks passed; all 48 negative titles built without diagnostics.
- The no-flush and cache-range-end variants failed their required host checks as intended. The new wait-any
  variant is compiled, but its expected pending-wait failure still requires the console.
- All 54 retained log hashes match, including the unchanged diagnostics v2 source log.
- Rebuilt `build/m55_check.xex` SHA-256: `e8a1506afcc0e0dbada5f9ff73359f78898027f2ffdb3df89edefb7b6b11e17c`.
- Local build/check logs: `local/m55/diagnostics-v3-clean-build.log`, `diagnostics-v3-checks.log` and
  `diagnostics-v3-breaks.log`. V3 has not run on hardware; no hardware acceptance is inferred from these builds.
