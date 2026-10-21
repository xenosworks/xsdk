# Milestone 55: Windows-style threading and memory

Agreed on 2026-10-20, with every recommendation the proposal made (Decided, below).

The gap docs/PARITY.md ranks sixth, "Threading for Windows-style code", with the Memory rows beside it: what code
written for Windows, and emulators, expect of threads and pages. Every piece is a kernel export Xenia implements, but
running code a title wrote, which only the console can answer. It can run beside milestone 54: both add to
imports/imports.txt and include/xsdk/kernel.h, and touch nothing else in common.

## Why

What porters and emulators hit today:
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
- **Running written code.** A dynamic recompiler writes PowerPC code and runs it. On RGH a title's own code pages can
  be: the user's PlayStation emulator pcsx360, run on this console, holds its recompiler's 4 MB code pool in a `.jit`
  image section, writes blocks there and runs them. Its log reads `[rec] code cache @82404200 (4 MB, .jit image
  section)` then `[rec] recompiler installed as psxCpu`
  (local/m55/pcsx360/p1log.txt:36-37, the user's title run on this console), and XexTool's section list shows those
  pages, 0x82400000 up, marked Code (local/m55/pcsx360/xextool_l.txt). Its codegen self-test, "dynarec codegen spike v8
  (RGH: RWX image section)", writes a function returning 42 into that section and calls it, then regenerates one
  returning 99 and calls again, and reports exec found. So executable memory on RGH is a `.jit` section in the title's
  image, its pages marked Code by the packer, not a page the title protects at run time. A retail console runs no
  unsigned title at all, so this is RGH and JTAG behaviour, like everything xsdk builds.
  - The way not taken: `NtProtectVirtualMemory` to an execute protection. Xenia drops the execute bits and refuses
    them there (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:96-99, 207-210); with the image-section route
    proven, a run-time protection change isn't needed, and `XexReserveCodeBuffer`/`XexCommitCodeBuffer` (0x36B, 0x36C,
    no code in Xenia: xboxkrnl_table.inc:889-890) stay unused.
  - The toolchain: `__builtin___clear_cache` compiles to nothing on xsdk's target, and libgcc's `__clear_cache` is a
    bare `blr` (checked with ~/xsdk-gcc on 2026-10-20). A JIT's cache flush would silently do nothing, so a title
    flushes its own, as pcsx360 and mupen64-360 do (mupen64-360:source/r4300/Recomp-Cache.c:301,
    libxenon:drivers/ppc/cache.S:55-70).

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

2. **A probe of running written code,** `tests/probe_exec.c`, pcsx360's method. It writes a function into a `.jit`
   image section, flushes the caches, calls it, rewrites it and calls again, as the user's emulator does on this
   console. No run-time protection change, and no fault handler: the `.jit` pages are already executable, so a correct
   build doesn't fault.
   - **The `.jit` section.** src/xsdk.ld gives `.jit` its own 64 KB-aligned region, filled from a `.jit` input section
     the title defines with alloc and execinstr flags; its size is the title's own (the probe's 64 KB). elf2xex marks
     an execinstr section's pages Code already (tools/elf2xex.py:143-144), so those pages come up executable, as
     pcsx360's do in the section list. elf2xex needs no change.
   - **The cache flush,** `xsdk_code_flush(start, bytes)` the probe carries, over 128-byte lines (the Xenon cache line,
     libxenon:drivers/ppc/cache.S:1): `dcbst` each line, `sync`, `icbi` each line, `sync`, `isync`. That is the
     self-modifying-code sequence the PowerPC architecture requires, data pushed from the data cache and the stale
     instructions dropped before the fetch (PowerPC Virtual Environment Architecture, Book II, the self-modifying-code
     programming note; PowerPC Microprocessor Family: The Programming Environments, instruction-cache management).
     mupen64-360 and libxenon do the same ops (mupen64-360:source/r4300/Recomp-Cache.c:301, libxenon:cache.S:55-70).
   - **Each step logs before it runs,** and the run has `net_log.txt`, so a line reaches the PC even if a step hangs
     (docs/HARDWARE.md, "The log over the network, milestone 53"). A build that runs on this console won't, as pcsx360
     shows, but the logging costs nothing and guards a console that differs.
   - **One case a run,** named in `game:\probe_exec.txt` beside the `.xex`, as tests/probe_fault.c takes its mode; with
     no file, the default is the whole spike. The cases:
     1. `query`: `NtQueryVirtualMemory` and `MmQueryAddressProtect` on the `.jit` section, the title's code, its data
        and the heap, logged. Nothing runs.
     2. `spike`, the default: write `li r3, 42; blr` into the `.jit` section, flush, call, check 42; rewrite to
        `li r3, 99; blr`, flush, call, check 99; and a call without the flush between, to show the flush is needed.

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
   - `munmap` decommits its range, and releases the reservation once `NtQueryVirtualMemory` shows none of it committed;
   - `mprotect` through `NtProtectVirtualMemory`: `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`; `PROT_EXEC`
     refused with `ENOTSUP`, as executable memory is the `.jit` section (step 7), not a run-time protection change;
   - `sbrk` decommits whole 64 KB steps above the break when newlib's `malloc` shrinks the heap, so memory freed at the
     heap's top goes back to the kernel;
   - `NtQueryVirtualMemory`, `MmSetAddressProtect` and `MmQueryAddressProtect` raw in kernel.h, for code that queries
     pages and for GPU memory.

7. **Executable memory, a `.jit` section,** as step 2's probe runs it:
   - src/xsdk.ld gives `.jit` its own 64 KB-aligned region; a title defines a `.jit` input section of the size it wants,
     alloc and execinstr, so elf2xex marks its pages Code and the console maps them executable, as pcsx360's are.
   - `xsdk/jit.h`: `xsdk_jit_pool(&base, &bytes)`, the section's extent, and `xsdk_code_flush(start, bytes)`, the
     cache sequence step 2 proved. A title lays its recompiler's code cache over the pool and flushes each block before
     it runs it.
   - `__builtin___clear_cache` and libgcc's `__clear_cache` are made to flush too (Decided, 4), so library code that
     calls the builtin works; `gcc-xsdk.patch`, the toolchain rebuilt, waits for me (CLAUDE.md, the toolchain note).
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
  signal-and-wait sets one event and wakes on another.
- **Events:** a cleared event stays clear; a pulse wakes the threads waiting on a manual-reset event and leaves it
  clear.
- **Mutants:** the owner takes one twice, and another thread waits until it's released twice; another thread's release
  fails with `STATUS_MUTANT_NOT_OWNED`; a wait on one whose owner ended holding it returns abandoned, if step 1 showed
  that safe.
- **Timers:** a one-shot timer signals once, at its time to within a tick; a periodic one fires the expected count in
  100 ms; a cancelled one doesn't fire.
- **Lock-free lists:** two threads push 1,000 entries each, and pops give back all 2,000, each once; a flush empties
  the list.
- **Semaphores:** `sem_post` on one thread wakes `sem_wait` on another; `sem_trywait` at zero fails with `EAGAIN`;
  `sem_timedwait` fails with `ETIMEDOUT` close to its deadline; `sem_getvalue` gives the count.
- **Priorities:** each level from `sched_get_priority_min` to `max` reads back; of two busy threads on one hardware
  thread, the raised one gets the share step 1 measured. These replace the check that priorities refuse a change.
- **Placement:** `xsdk_thread_place` moves another thread, busy and asleep in a wait, which then reports running there;
  `xsdk_pthread_handle` lets it move a `std::thread`.
- **Pages:**
  - an anonymous mapping comes back zeroed and takes writes, and `munmap` gives its pages back, `MmQueryStatistics`'
    free pages rising by its size;
  - `mprotect` read-only reads back as read-only from `NtQueryVirtualMemory` and still reads, then takes a write once
    read-write again, `PROT_EXEC` refused with `ENOTSUP`;
  - `MmSetAddressProtect` on GPU memory reads back through `MmQueryAddressProtect`;
  - a file mapping and `MAP_FIXED` are refused;
  - 32 MB freed at the heap's top raises the free pages by as much.
- **Written code:** a function written into the `.jit` section, flushed with `xsdk_code_flush`, runs and returns 42;
  rewritten and flushed, it returns 99. The break leaves the flush out, so the stale 42 runs and the 99 check fails.

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
3. **Executable memory is a `.jit` image section,** its pages marked Code by the packer, as the user's pcsx360 proves
   on this console (local/m55/pcsx360). The probe confirms it in xsdk's own build, writing a function there, flushing
   and calling; no run-time protection change, no fault handler, no code-buffer or hypervisor route.
4. **The cache flush:** the probe and the feature carry `xsdk_code_flush`, the PowerPC self-modifying-code sequence;
   and gcc-xsdk.patch makes `__builtin___clear_cache` and libgcc's `__clear_cache` flush too, the toolchain and
   everything on it rebuilt. The toolchain change waits for me, after the probe (CLAUDE.md, the toolchain note).
5. **Priorities on by default,** with Dusklight's title scene and SDL-xenon's sdl_check run once; if Dusklight is
   slower, Aurora's requests change, not xsdk.
6. **The heap:** `sbrk` gives back the heap's top, and `mmap` serves big buffers freed in the middle; no new `malloc`.

## Implementation follow-up (2026-10-20)

Steps 1 and 2 are recorded in `results/2026-10-20-milestone-55/`. Steps 3�8 now have SDK code, console checks and
usage documentation; console acceptance remains pending. The user explicitly retained GCC's patch and rebuild.
Two changes follow the actual probe evidence:

- `NtQueryVirtualMemory` failed with C000000D. `mmap` tracks its reservations and committed-page bits; `munmap`
  decommits live subranges and releases after the last page. Protection checks use `MmQueryAddressProtect`.
  The raw query stays declared with its console limitation documented; its signature is not guessed or silently
  treated as successful. The resolved query and broader raw query coverage remain an open hardware investigation.
- An unflushed rewrite was visible on this console. A no-flush build remains diagnostic, with a compiled-instruction
  check that must reject it. Wrong-code variants deterministically exercise the hardware return-value checks.

`NtDuplicateObject` (Xenia xboxkrnl_ob.cc) is also imported: pthread_self on the main thread or a foreign thread
needs a real owned handle, so its pthread identity can safely be used by another thread. A pseudo handle would
refer to the new caller instead. Created pthreads retain ExCreateThread's handle until join or detached completion.

See the results README for builds, expected negative checks and the remaining hardware acceptance runs. Nothing
here marks the milestone done before those runs.
