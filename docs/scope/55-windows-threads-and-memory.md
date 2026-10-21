# Milestone 55: Windows-style threading and memory

Agreed on 2026-10-20, with every recommendation the proposal made (Decided, below).

The gap docs/PARITY.md ranks sixth, "Threading for Windows-style code", with the Memory rows beside it: what code
written for Windows, and emulators, expect of threads and pages. Every piece is a kernel export Xenia implements, but
running code a title wrote, which only the console can answer. M55 is rebased onto milestone 54; their build,
header, import and guide additions are retained together.

## Why

The gaps when this milestone was proposed:
- **Waits on one object at a time.** xsdk imports `NtWaitForSingleObjectEx` alone (imports/imports.txt). Windows code
  waits on several handles at once, resets and pulses events, takes owned mutexes and sets waitable timers; none of
  that links, so each port writes its own stand-ins on events and critical sections.
- **No priorities.** `pthread_setschedparam` returns `ENOTSUP` (src/pthread.c). SDL3 sets priorities through it
  (SDL-xenon:src/thread/pthread/SDL_systhread.c:188-282), so every `SDL_SetCurrentThreadPriority` fails today:
  Dusklight asks for its threads' priorities that way (dusklight-xenon:extern/aurora/lib/thread.cpp:345), and SDL's
  audio thread asks for time-critical (SDL-xenon:src/audio/SDL_audio.c:850).
- **Only the calling thread moves.** `xsdk_thread_move` moves its caller (src/thread.c). A port that places its workers
  from the main thread, by handle, can't.
- **No `sem_*`.** newlib has no `<semaphore.h>` for this target, so SDL-xenon builds SDL's generic semaphore
  (SDL-xenon:CMakeLists.txt:3321-3324), and ports using POSIX semaphores don't link.
- **Memory never goes back.** `mmap`, `munmap` and `mprotect` don't link (docs/GUIDE.md, "Porting"). newlib's `malloc`
  hands the top of its heap back through `sbrk` with a negative size once 128 KB is free there
  (newlib:newlib/libc/stdlib/_mallocr.c:765), but xsdk's `sbrk` only moves the break (src/sbrk.c). A title's peak stays
  committed until it exits, and `xsdk_gpu_alloc` can't have those pages.
- **Running written code.** xsdk's own `probe_exec` wrote a function into its `.jit` image section, flushed it,
  and called it on this RGH console: 42, then 99 after rewriting and flushing again
  ([probe_exec_spike.log](../../results/2026-10-20-milestone-55/probe_exec_spike.log)). The linker aligns the section
  to 64 KB and the packer marks its pages Code. This establishes the image-section method on the tested console.
  - The way not taken: `NtProtectVirtualMemory` to an execute protection. Xenia drops the execute bits and refuses
    them there (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:96-99, 207-210); with the image-section route
    proven, a run-time protection change isn't needed, and `XexReserveCodeBuffer`/`XexCommitCodeBuffer` (0x36B, 0x36C,
    no code in Xenia: xboxkrnl_table.inc:889-890) stay unused.
  - The toolchain: `__builtin___clear_cache` compiles to nothing on xsdk's target, and libgcc's `__clear_cache` is a
    bare `blr` (checked with ~/xsdk-gcc on 2026-10-20). A JIT's cache flush would silently do nothing, so a title
    uses `xsdk_code_flush` explicitly until the cache-flush work in [TODO.md](../../TODO.md) is complete.

What Xenia has, under xenia:src/xenia/kernel/ (ordinals from xboxkrnl/xboxkrnl_table.inc):

| What | Exports | Xenia |
|---|---|---|
| Waits | `NtWaitForMultipleObjectsEx` 0xFE, `NtSignalAndWaitForSingleObjectEx` 0xFB | xboxkrnl_threading.cc:856-912; any count; wait type 0 is all, 1 any, which returns the index (xobject.cc:254-304) |
| Events | `NtClearEvent` 0xCE, `NtPulseEvent` 0xE2 | xboxkrnl_threading.cc:500-536; a pulse always reports the event was set (xevent.cc:65-68) |
| Owned mutexes (mutants) | `NtCreateMutant` 0xD4, `NtReleaseMutant` 0xF2 | xboxkrnl_threading.cc:638-696; the release's second argument asserted 0 and ignored |
| Timers | `NtCreateTimer` 0xD7, `NtSetTimerEx` 0xFA, `NtCancelTimer` 0xCD | xboxkrnl_threading.cc:698-777; eight arguments, the fourth asserted 1 and the eighth 0 |
| Priorities | `KeSetBasePriorityThread` 0x99, `KeQueryBasePriorityThread` 0x81 | xboxkrnl_threading.cc:260-284, mapped onto five host levels at ±0x11 and ±0x22 (xthread.cc:719-736): a guess |
| A thread by its handle | `ObReferenceObjectByHandle` 0x110, `ObDereferenceObject` 0x105, `ExThreadObjectType` 0x1B | xboxkrnl_ob.cc:84-149; 0xFFFFFFFE is the calling thread (util/object_table.cc:322-328) |
| Lock-free lists | `InterlockedPushEntrySList` 0x2D, `InterlockedPopEntrySList` 0x2C, `InterlockedFlushSList` 0x2B | xboxkrnl_threading.cc:1320-1392, an 8-byte header swapped whole (xenia:src/xenia/xbox.h:284-289) |
| Pages | `NtFreeVirtualMemory` 0xDC, `NtProtectVirtualMemory` 0xE1, `NtQueryVirtualMemory` 0xEE, `MmSetAddressProtect` 0xC7, `MmQueryAddressProtect` 0xC4 | xboxkrnl_memory.cc:193-331, 413-437 |
| APCs | `NtQueueApcThread` 0xE3 | xboxkrnl_threading.cc:1013-1031 |
| Completion ports | `NtCreateIoCompletion` 0xD3, `NtSetIoCompletion` 0xF8, `NtRemoveIoCompletion` 0xF4 | xboxkrnl_io.cc:432-503, a queue nothing posts to |

Xenia runs each title thread on a host thread of its own (CLAUDE.md), so it can't show what a priority does on one
hardware thread either.

## What it takes

Each step rests on the console where Xenia can't answer. Steps 1 and 2 are the probes; 3 to 7 build on what they find.

1. **A probe of the objects, threads and pages,** `tests/probe_objects.c`, one run. It settles:
   - **waits:** wait-all and wait-any over events, a semaphore, a mutant and a thread; the most objects one wait takes;
   - **events:** what a pulse wakes, on a manual-reset event and an auto-reset one, and the previous state it reports;
   - **mutants:** a second take by the owner, the status of a release by another thread, and `NtReleaseMutant`'s
     second argument;
   - **timers:** `NtSetTimerEx`'s arguments; one-shot timers of 1 and 5 ms and periodic ones of 1, 2 and 16 ms, timed
     with the time base, against a 1 ms sleep's 2 ms (docs/HARDWARE.md, "Sleeps, write speeds, and a set-up that
     wouldn't run from a cached stream");
   - **priorities:** what `KeSetBasePriorityThread` takes and `KeQueryBasePriorityThread` gives back, from -16 to 16;
     then two busy threads on hardware thread 1, one raised a level at a time, and each one's share of the work. Each
     stops by the time base after 300 ms, so a starved thread costs 300 ms, not a hang;
   - **moving another thread:** `ObReferenceObjectByHandle` on a thread's handle and on 0xFFFFFFFE, then
     `KeSetAffinityThread` on a busy thread and on one asleep in a wait, each reporting where it runs next;
   - **pages:**
     - part of a reservation decommitted, then all of it released, with `MmQueryStatistics` before and after;
     - `NtQueryVirtualMemory` on committed, reserved and free pages, against Xenia's layout
       (xboxkrnl_memory.cc:288-296);
     - `NtProtectVirtualMemory` to read-only and no access and back, on 4 KB pages, 64 KB pages and 4 KB of a 64 KB-page
       allocation, and the old protection it reports;
     - a write to a read-only page, caught and resumed by the vectored handler milestone 40's probe used
       (tests/probe_fault.c);
     - `MmSetAddressProtect` and `MmQueryAddressProtect` on `xsdk_gpu_alloc`'s memory, put back after.

   Last, each logged before it runs, as a kernel that takes either as fatal would stop the console there: a wait on
   one object more than the most, and a wait on a mutant whose owner ended holding it.

2. **A probe of running written code,** `tests/probe_exec.c`. It writes a function into a `.jit`
   image section, flushes the caches, calls it, rewrites it and calls again. It uses no run-time protection change
   or fault handler; the spike log records successful execution from the image section.
   - **The `.jit` section.** src/xsdk.ld gives `.jit` its own 64 KB-aligned region, filled from a `.jit` input section
     the title defines with alloc and execinstr flags; its size is the title's own (the probe's 64 KB). elf2xex marks
     an execinstr section's pages Code already (tools/elf2xex.py, `section_type`). elf2xex needs no change.
   - **The cache flush:** the probe's `code_flush` becomes the SDK's `xsdk_code_flush(start, bytes)`, over 128-byte
     lines (libxenon:drivers/ppc/cache.S:1): `dcbst` each line, `sync`, `icbi` each line, `sync`, `isync`. The
     cache ordering comes from [PowerPC Book II v2.01, section 1.8, p. 11](https://www.ece.lsu.edu/ee4720/doc/ppc_isa_book2.pdf#page=19).
     The second `sync` orders invalidation before publication; `isync` synchronizes the executing thread.
     A different consumer must observe publication and then execute its own `isync` before calling the code.
   - **Each step logs before it runs,** and `net_log.txt` can send the last line to the PC if a step hangs
     (docs/HARDWARE.md, "The log over the network, milestone 53").
   - **One case a run,** named in `game:\probe_exec.txt` beside the `.xex`, as tests/probe_fault.c takes its mode; with
     no file, the default is the whole spike. The cases:
     1. `query`: `MmQueryAddressProtect` on `.jit`, logged. `probe_query_memory` separately tests the three-argument
        virtual-memory query and its refusal of image pages; nothing generated runs in this mode.
     2. `spike`, the default: write `li r3, 42; blr` into the `.jit` section, flush, call, check 42; rewrite to
        `li r3, 99; blr`, flush, call, check 99; and an unflushed rewrite as a diagnostic. Logging may evict the
        previous code, so this does not isolate cache effects.

   Cached memory only; the `.jit` section is in the image, not GPU memory. `XexReserveCodeBuffer` and
   `XexCommitCodeBuffer` aren't called, and nothing reaches the hypervisor (CODE_GUIDELINES.md 1.2).

3. **The objects,** imported and declared raw in kernel.h, as milestone 51's XAM calls are: both waits, `NtClearEvent`,
   `NtPulseEvent`, the mutant calls, the timer calls and the lock-free lists, with `SLIST_HEADER`, the wait types and
   the statuses (`STATUS_ABANDONED_WAIT_0`, `STATUS_MUTANT_NOT_OWNED`). Their results are Windows' own:
   `STATUS_WAIT_0` plus the index, `STATUS_ABANDONED_WAIT_0` (0x80) and `STATUS_TIMEOUT` (0x102) are `WAIT_OBJECT_0`,
   `WAIT_ABANDONED_0` and `WAIT_TIMEOUT` (xenia:src/xenia/xbox.h:36-39). So a port's own Win32 layer maps each call to
   one export. Timers without APC routines (Left out).

4. **Threads,** in src/thread.c, src/pthread.c and include/xsdk/thread.h:
   - `xsdk_thread_place(thread, hardware_thread)` moves any thread by its handle, through `ObReferenceObjectByHandle`
     and `KeSetAffinityThread`. `XSDK_CURRENT_THREAD`, 0xFFFFFFFE, is the caller, if step 1 shows the kernel takes it;
   - `xsdk_thread_set_priority(thread, priority, &previous)`, through `KeSetBasePriorityThread`, at the levels step 1
     finds;
   - `xsdk_pthread_handle(pthread)`, a pthread's kernel handle, so a `std::thread` can be placed, raised and waited on
     with other objects. Every pthread then keeps its handle until it's joined, or, detached, until it ends;
   - `pthread_setschedparam` and `pthread_getschedparam` on those levels, any policy, `sched_get_priority_min` to `max`
     symmetric about 0, the default. SDL's normal priority is the range's midpoint (SDL_systhread.c:255-273), so it
     stays the kernel's default.

5. **POSIX semaphores,** in include/semaphore.h and src/semaphore.c: `sem_init`, `sem_destroy`, `sem_wait`,
   `sem_trywait`, `sem_timedwait`, `sem_clockwait`, `sem_post` and `sem_getvalue`, on `NtCreateSemaphore`,
   `NtReleaseSemaphore` and `NtWaitForSingleObjectEx`. `sem_getvalue` reads a count kept beside the handle, as
   `NtQuerySemaphore` has no code in Xenia (xboxkrnl_table.inc:249) and nothing else needs it.

6. **Pages,** in include/sys/mman.h, src/mman.c and src/sbrk.c:
   - `mmap` of anonymous memory, `MAP_PRIVATE | MAP_ANONYMOUS`, through `NtAllocateVirtualMemory`; a file, `MAP_FIXED`
     or `MAP_SHARED` refused;
   - `munmap` decommits its range, and releases the reservation once its tracked committed pages are all gone;
   - `mprotect` through `NtProtectVirtualMemory`: `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`; `PROT_EXEC`
     refused with `ENOTSUP`, as executable memory is the `.jit` section (step 7), not a run-time protection change;
   - `sbrk` decommits whole 64 KB steps above the break when newlib's `malloc` shrinks the heap, so memory freed at the
     heap's top goes back to the kernel;
   - `MmSetAddressProtect` and `MmQueryAddressProtect` raw in kernel.h for GPU memory and protection queries.
     `NtQueryVirtualMemory(address, information, 0)` queries regions from `NtAllocateVirtualMemory`/`mmap`; image
     pages return C000000D, so use `MmQueryAddressProtect` for them (query follow-up below).

7. **Executable memory, a `.jit` section,** as step 2's probe runs it:
   - src/xsdk.ld gives `.jit` its own 64 KB-aligned region; a title defines a `.jit` input section of the size it wants,
     alloc and execinstr, so elf2xex marks its pages Code and the console maps them executable, as the spike log records.
   - `xsdk/jit.h`: `xsdk_jit_pool(&base, &bytes)`, the section's extent, and `xsdk_code_flush(start, bytes)`, the
     cache sequence step 2 proved. A title lays its recompiler's code cache over the pool and flushes each block before
     it runs it.
   - `__builtin___clear_cache` and libgcc's `__clear_cache` are made to flush too (Decided, 4), so library code that
     calls the builtin works; the patch and rebuild remain tracked in [TODO.md](../../TODO.md).
   - `mmap` and `mprotect` don't make memory executable; `PROT_EXEC` is `ENOTSUP` (step 6).

   docs/HARDWARE.md records the probe's result.

8. **Docs:** docs/GUIDE.md's threads and porting sections, with a raised busy thread starving the rest of its hardware
   thread; docs/PARITY.md's Threads and Memory rows; docs/HARDWARE.md for both probes; CLAUDE.md's Xenia note for the
   checks below that Xenia can't pass.

## Left out

- **APCs and alertable waits:** `NtQueueApcThread`, timers' APC routines, XAM's completion routines. An event does the
  same for a port, xsdk never waits alertably, and milestone 51 took XAM's results through events for that reason.
- **I/O completion ports:** nothing in xsdk completes into one, as files open synchronous (src/file.c) and sockets are
  XAM's. As a work queue, a semaphore and a lock-free list do it.
- **A Win32 layer** (`windows.h`, `CreateEvent`, `WaitForMultipleObjects`, `VirtualAlloc`), until a port asks
  (Decided, 1).
- **Named objects and opening them by name, suspending and resuming threads, starting one suspended**
  (`xsdk_thread_create_on` places a thread as it starts), **priority boosts, priorities in thread attributes, named
  semaphores** (`sem_open`).
- **File mappings, `MAP_FIXED`, `MAP_SHARED`, guard pages, `madvise`, `mlock`.**
- **A new `malloc`.** newlib's has no `mmap` path (`HAVE_MMAP` 0, _mallocr.c:311), so only the heap's top goes back:
  memory freed below a live block waits for the blocks above it. A replacement would be a library added.
- **The heap on 64 KB pages,** and mutexes holding their own critical section (TODO.md), each a change of its own.
- **`std::counting_semaphore` on `sem_t`.** libstdc++ was built without POSIX semaphores
  (`_GLIBCXX_HAVE_POSIX_SEMAPHORE` unset in the toolchain's bits/c++config.h) and keeps its atomic waits; SDL-xenon
  keeps its generic semaphore.
- **`XexReserveCodeBuffer` and `XexCommitCodeBuffer`,** run-time executable protection through `NtProtectVirtualMemory`,
  and any route to running code through the hypervisor: executable memory is the `.jit` image section instead.

## Verification

On the console, each check its own line in xsdk_test, each broken on purpose once through tests/breaks/, as milestone
51's are:
- **Waits:** a wait on three events returns the index of the one set; a wait for all three times out with two set and
  returns once the third is; a wait on the most objects works and one more is refused, as step 1 found; a
  signal-and-wait sets one event and wakes on another. Mixed event/semaphore/mutant/thread wait-all is checked
  both with a completed thread and with a thread held behind a gate until the waiter has had time to block.
- **Events:** a cleared event stays clear; a pulse wakes the threads waiting on a manual-reset event and leaves it
  clear.
- **Mutants:** the owner takes one twice, and another thread waits until it's released twice; another thread's release
  fails with `STATUS_MUTANT_NOT_OWNED`; a wait on one whose owner ended holding it returns abandoned, if step 1 showed
  that safe.
- **Timers:** a one-shot timer signals once, at its time to within a tick; 20 periodic firings allow 2 ms per
  requested 5 ms period (60–140 ms total), matching the one-shot tolerance; a cancelled one doesn't fire.
- **Lock-free lists:** two threads push 1,000 entries each, and pops give back all 2,000, each once; a flush empties
  the list.
- **Semaphores:** `sem_post` on one thread wakes `sem_wait` on another; `sem_trywait` at zero fails with `EAGAIN`;
  `sem_timedwait` fails with `ETIMEDOUT` close to its deadline; `sem_getvalue` gives the count.
- **Priorities:** every request from `sched_get_priority_min` to `max` queries back the measured kernel result:
  -16/+16 unchanged, interior requests clamped to -3..4 on the tested title thread (diagnostics v2, below).
  The original priority is restored. Of two busy threads on one hardware thread, the raised one gets the share
  step 1 measured. These replace the check that priorities refuse a change.
- **Placement:** `xsdk_thread_place` moves another thread, busy and asleep in a wait, which then reports running there;
  `xsdk_pthread_handle` lets it move a `std::thread`.
- **Pages:**
  - an anonymous mapping comes back zeroed and takes writes, and `munmap` gives its pages back, `MmQueryStatistics`'
    free pages rising by its size;
  - `mprotect` read-only reads back as read-only from `MmQueryAddressProtect` and still reads, then takes a write once
    read-write again, `PROT_EXEC` refused with `ENOTSUP`;
  - `MmSetAddressProtect` on GPU memory reads back through `MmQueryAddressProtect`;
  - a file mapping and `MAP_FIXED` are refused;
  - 32 MB freed at the heap's top raises the free pages by as much.
- **Written code:** a function written into the `.jit` section, flushed with `xsdk_code_flush`, runs and returns 42;
  rewritten and flushed, it returns 99. Cross-line and 129-byte cases check execution after flushing wider ranges.
  Host traces of the production C loops check exactly which lines and barriers execute, including zero bytes.
  The no-flush and end-off-by-one breaks must fail host checks; wrong-code breaks test console return checks.

Also vulkan4xenon_test twice and vkbench, unchanged; and SDL-xenon's sdl_check and Dusklight's title scene once each,
as SDL's priority requests now reach the kernel: the scene's frame rate no lower than before.

Xenia can't pass the priority share, the moved threads' placement, or the pages given back, as its `MmQueryStatistics`
guesses; CLAUDE.md will say so.

## Done when

- Each item above works on the console, its check passing and its break failing.
- docs/HARDWARE.md records both probes: the objects, priorities and pages, and the `.jit` section running written
  code.
- docs/GUIDE.md and docs/PARITY.md say what's there: the Threads rows Yes, APCs and completion ports aside; the Memory
  row Yes; `mmap`, `munmap`, `mprotect` and `sem_*` off the guide's list of what doesn't link.
- Dusklight's title scene runs no slower with its priorities honoured.

## Decided

1. **The API's shape:** POSIX where POSIX has the call (`sem_*`, `mmap`, `munmap`, `mprotect`,
   `pthread_setschedparam`); the Windows-style objects as raw exports in kernel.h, which take Win32's arguments and
   return its wait results; and three xsdk calls where the kernel is awkward, `xsdk_thread_place`,
   `xsdk_thread_set_priority` and `xsdk_pthread_handle`. Thin xsdk calls for the waits, or a `windows.h`, can come
   when a port asks.
2. **Timers in,** without APC routines; APCs and completion ports out.
3. **Executable memory is a `.jit` image section,** its pages marked Code by the packer. xsdk's own
   [spike log](../../results/2026-10-20-milestone-55/probe_exec_spike.log) records generated code returning 42 and 99.
   No run-time protection change, fault handler, code-buffer or hypervisor route is used.
4. **The cache flush:** the probe and the feature carry `xsdk_code_flush`, the PowerPC self-modifying-code sequence;
   and gcc-xsdk.patch makes `__builtin___clear_cache` and libgcc's `__clear_cache` flush too, the toolchain and
   everything on it rebuilt. The toolchain work remains in [TODO.md](../../TODO.md).
5. **Priorities on by default,** with Dusklight's title scene and SDL-xenon's sdl_check run once; if Dusklight is
   slower, Aurora's requests change, not xsdk.
6. **The heap:** `sbrk` gives back the heap's top, and `mmap` serves big buffers freed in the middle; no new `malloc`.

## Implementation follow-up (2026-10-20)

Steps 1 and 2 are recorded in `results/2026-10-20-milestone-55/`. Steps 3–8 now have SDK code, console checks and
usage documentation; console acceptance remains pending. GCC's patch and rebuild remain in [TODO.md](../../TODO.md).
Two changes follow the actual probe evidence:

- The initial two-argument `NtQueryVirtualMemory` calls failed with C000000D. The
  [query follow-up](../../results/2026-10-20-milestone-55/probe_query_memory.log) validates
  `(address, information, 0)` for committed, reserved and free virtual regions; its declaration and ordinal 0x0EE
  import are restored. Title code and `.jit` still return C000000D, so their protection queries use
  `MmQueryAddressProtect`. Only the zero third argument is established. `mmap` uses `NtAllocateVirtualMemory`
  and keeps its own reservations and committed-page bits for lifetime ownership; `munmap` decommits live
  subranges and releases after the last page.
- The unflushed rewrite returned the new value, with logging between the earlier call and rewrite. Incidental
  eviction was not excluded. Host checks reject missing cache maintenance; console wrong-code variants check returns.

`NtDuplicateObject` (Xenia xboxkrnl_ob.cc) is also imported: pthread_self on the main thread or a foreign thread
needs a real owned handle, so its pthread identity can safely be used by another thread. A pseudo handle would
refer to the new caller instead. Created pthreads retain ExCreateThread's handle until join or detached completion.

See the results README for builds, expected negative checks and the remaining hardware acceptance runs. Nothing
here marks the milestone done before those runs.


## Acceptance failures (2026-10-21)

The [first complete acceptance set](../../results/2026-10-21-milestone-55-acceptance/) is not a passing milestone.
The focused and full titles both fail periodic timing, mixed wait-all, all-level priority read-back and heap return.
The timer check now budgets lateness per expiration; the heap test now prevents GCC deleting its allocation and
checks that compiled malloc/free calls remain. Mixed-wait and priority assertions remain strict, with diagnostics
for the missing statuses and actual levels. All four need the revised focused console run before negative-test
acceptance can be claimed. The restored three-argument query's separate v2 run passed 22 of 22 checks.

## Diagnostics v2 follow-up (2026-10-21)

The [v2 console log](../../results/2026-10-21-milestone-55-acceptance/m55_check_diagnostics_v2.log) passed 59 checks
and failed the old expectation that every accepted priority request reads back unchanged. The revised timer check
passed at 118.561 ms for 20 firings. The heap check returned 8,200 pages after freeing the touched 32 MB allocation.
All priority requests and queries succeeded: -16/+16 read back unchanged; ordinary requests saturated at -3 and +4.
The SDK continues to expose the kernel's values; diagnostics v3 checks the exact measured mapping and restoration.

Mixed wait-all passed in 2.646 microseconds. Logging before the wait may have let its worker finish; this run does
not explain the earlier failures. V3 explicitly waits for a completed thread in the ready control and adds a
separate target held behind a gate, with no logging during the pending wait. A wait-any mutation must fail that
pending check. These changes still require a focused console run, then the full suite and affected negative runs.
