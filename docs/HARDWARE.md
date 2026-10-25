# Hardware results

Facts the user's RGH console has settled. Each entry says what ran, what happened, and when.

## XEX loading (2026-10-09)

Launched from Aurora, logs read through xbWatson and `xsdk.log`.

- **Accepted:** an unsigned, unencrypted XEX2 in retail format with BASIC compression (one raw block), written by
  `elf2xex`. A copy re-packed by XexTool as devkit, encrypted and uncompressed also runs.
- **Refused:** every earlier build. Their import thunks had zero in word 1. The working builds differ in four ways at
  once, so which one the loader needed isn't isolated:
  - thunks hold `01LLOOOO 02LLOOOO mtctr r11 bctr` and are 16-byte aligned;
  - import slots are in read-only data;
  - the imports claim 2.0.21256.0 with minimum 2.0.1861.0 (it was 2.0.17559.0 for both);
  - there's no default heap size header.
- **The working layout** matches an XDK-built homebrew title that runs on the same console.
  - Its import library IDs are `0xFCA15C76` for xam.xex and `0x45DC17E0` for xboxkrnl.exe.
  - It has one page descriptor per 64 KB page.
  - Its import libraries header ends the XEX headers.

## XEX checks the kernel enforces (2026-10-09)

`tests/xex_checks.py` copies the working title with one digest broken by a single flipped byte:

| Broken digest | Result |
|---|---|
| Header digest (security info +0x164) | Launches and passes every test, so not enforced |
| Page digest (a byte nothing reads, in page 0) | Launches and passes every test, so not enforced |
| Import table digest (security info +0x12C) | Refused, so enforced |

An xbWatson minidump's system information reports build 21256, but XAM's D3D hang report on the same console says
kernel 17559 (both 2026-10-10), so which number is the kernel's isn't settled. The minidump's module list names the
kernel `xboxkrnld.exe` and XAM `xamd.dll`, with `Xbdm.xex` loaded. The patch set isn't recorded; another patch set may
differ.

## Runtime (2026-10-09)

The test title (`tests/main.c`) ran and logged:

- `game:` with root `OB_DOS_DEVICES` resolves to the folder the title was launched from, and it's writable.
- Read-only data, initialised data and zeroed data are loaded and laid out as linked. Data is writable.
- Kernel calls from GCC's 32-bit code work through `tools/gen_imports.py` stubs:
  - `NtCreateFile`, which takes 9 arguments, so arguments past the eighth reach the kernel from `r1+0x54`;
  - `NtWriteFile`;
  - `NtClose`;
  - `NtAllocateVirtualMemory`, which newlib's `malloc` reaches through `sbrk`;
  - the variadic `DbgPrint`.
- newlib's printf formats correctly. The two stray NUL bytes per conversion seen in Xenia come from Xenia skipping
  `mcrf`.
- `XamLoaderTerminateTitle` returns to the dashboard and doesn't come back.

## Runtime, milestone 2 (2026-10-09)

The test title passed all 12 checks on the console:

- **Threads.** `ExCreateThread` with an XAPI startup routine calls it with the start address and context, as in
  Xenia (xthread.cc:518-521). `ExTerminateThread` ends the thread, and `NtWaitForSingleObjectEx` on its handle
  returns.
- **Synchronisation.** Two semaphores and a synchronization event hand 100 items from one thread to another (sum 5050).
  Two threads log through the critical-section-guarded log without corrupting it.
- **Files through newlib.** `fopen`, `fputs`, `fgets`, `fseek`, `ftell` and append mode work on
  `game:\xsdk_file_test.txt` through the SDK's `open`, `read`, `write`, `lseek` and `fstat`.
- **Controller.** `XamInputGetState(0, 0, &state)` returns live state:
  - the packet number advances only when the state changes;
  - sticks are signed 16-bit over the full range;
  - triggers run 0-255;
  - buttons are a bit mask, for example `0x0200` and `0xA200`.
- **Audio.**
  - The render driver calls the client callback 376 times in 2000 ms. That's one call per 256-sample frame at
    48 kHz, matching Xenia's frame size and rate.
  - The callback may call `XAudioSubmitRenderDriverFrame` itself.
  - `MmAllocatePhysicalMemoryEx` returns usable memory for the frame.
  - The frame layout plays as intended. Checked by ear on a stereo TV with `audio_check.xex`:
    - channel 0 sounds from the left speaker only, channel 1 from the right only;
    - the centre and back channels sound as a stereo downmix allows;
    - a 750 Hz tone sounds an octave above 375 Hz.
    So frames are 6 channels of big-endian floats, channel after channel, in Xenia's 5.1 order (front left, front
    right, centre, LFE, back left, back right).

## C++23 runtime, milestone 3 (2026-10-10)

The test title, built with the GCC 15 toolchain, passed all 30 checks on the console; the log is in
`results/2026-10-10-milestone-3/xsdk.log`.

- **Start-up.** A static constructor in `.init_array` runs before `main`. 64-byte, 16-byte and page-aligned
  allocations come back aligned.
- **Threads.** Two `std::jthread`s rally 100 times each through a `std::mutex` and a `std::condition_variable`, and see
  their own `thread_local` values (emulated TLS on `KeTlsAlloc`/`KeTlsGetValue`/`KeTlsSetValue`).
- **Atomics.** Four threads counting 10,000 times each give exact 32-bit and 64-bit totals, so the inline
  `lwarx`/`stwcx.` and `ldarx`/`stdcx.` loops work. Over a 200 ms `std::atomic::wait` the waiter yielded 16 times and
  blocked once, so it sleeps.
- **Time.** A 250 ms `sleep_for` measured 249 ms and 250 ms on `steady_clock` in two runs. That was too coarse to show
  that `KeQueryPerformanceFrequency` overstates the time base's rate by 0.25%, nor that the sleep can end early (see
  "The time base runs at 49.875 MHz (2026-10-11)"). `system_clock` reads the right date from `KeQuerySystemTime`.
- **File system.** `std::filesystem` creates, lists and removes a directory under `game:`:
  - `NtCreateFile` with `FILE_CREATE` and `FILE_DIRECTORY_FILE` creates it;
  - `NtQueryDirectoryFile`, given the pattern `*` on the first call and none after, lists it;
  - `NtSetInformationFile` with `FileDispositionInformation`, then `NtClose`, deletes the file and then the empty
    directory. Xenia ignores this, so the console is the first proof.
- **Formatting.** `std::format`, `std::expected` and `std::println` work; `println` reaches the log through stdout.
- **Exceptions.** A throw is caught, an `exception_ptr` from another `std::jthread` rethrows, and `dynamic_cast` and
  `typeid` work. Before the unwinder register-size fix in `tools/toolchain/gcc-xsdk.patch`, the first throw ended in
  an access violation at a stack address, 0x7010FA4C, repeated as first-chance exceptions in xbWatson.

The rule that every `lwarx` pairs with a `stwcx.` (`tools/toolchain/gcc-xsdk.patch`) isn't observable in this run and
stays unverified.

## Command processor at title start (2026-10-10)

`tests/probe_cp.c` read the GPU registers from a title launched from Aurora, without halting the command processor
(log: `results/2026-10-10-probe-cp/xsdk.log`). Register meanings come from libxenon:drivers/xenos/xe.c.

- **Titles can read the GPU registers** at 0x7FC80000 under this kernel.
- **The command processor is running and idle at title start.** 0x07D8 is 0x00007FFF, so bit 28 (halted) is clear,
  and the busy bit of 0x1740 is clear. The ring registers describe XAM's ring, as a later run showed: base
  0x01B9D000, control 0x00020309, read pointer write-back at 0x01B9F03C, read and write pointers both 0xEB.
- **The ME microcode reads back** through 0x07E4 and 0x07E8 while the command processor runs: 1100 of 2304 words are
  non-zero, and two reads give the same CRC-32, 7A8E8248. libxenon's copy differs (1178 non-zero words, CRC-32
  4D1AD8C0), so the console's microcode is a different version.
- **Reading the PFP microcode while it runs hangs the GPU.** Two reads through 0x117C and 0x1180 disagree, with 250 of
  288 words non-zero (libxenon's PFP has every word non-zero). Afterwards 0x1740's busy bit was set, 0x07D8 had gained
  bit 29, and once the title exited, debug XAM broke into the debugger with "The GPU is hung and can't be recovered
  without doing a cold boot."

A second run halted the command processor first, as libxenon does: it set bit 28 of 0x07D8 and waited 2 ms before
reading, then wrote back the original value (log: `results/2026-10-10-probe-cp-halted/xsdk.log`).

- **Halting works.** 0x07D8 then reads 0x10007FFF, and the busy bit stays clear.
- **The PFP microcode can't be read back, halted or not.** Two reads through 0x117C and 0x1180 disagreed again, with
  250 non-zero words, so a read of 0x1180 doesn't return the microcode.
- **The ME microcode read the same as in the first run:** 1100 non-zero words, CRC-32 7A8E8248.
- **Touching the PFP microcode RAM wrecks the command processor either way.** After resuming, 0x07D8 read back as
  0x22... (the log ends mid-line), the GPU hung, and XAM again asked for a cold boot.

So SCOPE.md section 3.2's option 1 fails: there's no way to read back the PFP microcode that `VdInitializeEngines`
needs. Reading the ME RAM while the command processor runs turned out to hang the GPU too (see "XAM's D3D device
shares the command processor"), so the probe no longer reads either.

## A ring without VdInitializeEngines (2026-10-10)

`tests/probe_ring.c` skipped `VdInitializeEngines` and used the command processor as it runs under a title, with its
microcode (log: `results/2026-10-10-probe-ring/xsdk.log`).

- **The kernel's ring set-up works on the running command processor.** After `VdInitializeRingBuffer(physical, 12)`
  and `VdEnableRingBufferRPtrWriteBack(physical, 6)`, 0x0700 holds the ring's physical address, 0x0704 reads 0x0002060C
  (size 12 in the low bits, block size 6 from bit 8), and 0x070C holds the write-back address with 2 ORed in.
- **`VdInitializeRingBuffer` leaves both ring pointers at 0xEB, not 0,** with the ring zeroed beforehand. The kernel
  appears to submit 235 dwords of its own; a title must append after the write pointer it finds.
- **The command processor runs a title's packets.** The probe wrote its packets at offset 0 and set the write pointer to
  9, so the command processor wrapped through the zeroed ring first. Even so, the `MEM_WRITE` landed, and with
  address bits 1-0 set to 2 the CPU reads the value as written. The read-pointer write-back reached 9.
- **The `EVENT_WRITE_SHD` fence didn't land** in that run, after about 8,000 zero dwords had each written GPU register
  0. The GPU ended busy, 0x07D8 gained bit 29, and XAM reported a hung GPU after the title exited.

A second run, after a cold boot, appended its packets at the write pointer the kernel left (log:
`results/2026-10-10-probe-ring-2/xsdk.log`).

- **The pointers moved again during set-up,** from 0xEB to 0x115 (the first run went from 0xC7 to 0xEB), and this time
  the GPU was already busy right after `VdEnableRingBufferRPtrWriteBack`, with 0x07D8 bit 29 set.
- **The CPU read back zeros for every dword before the new write pointer.** The probe had zeroed the write-combined
  ring and called `VdInitializeRingBuffer` without a `sync`, so the zeroing may have landed on top of packets the
  kernel wrote, which would explain both runs.
- **Neither fence landed,** though the read pointer reached the end of the probe's packets.
- **XAM's D3D printed a GPU hang report** after the title exited. It names 0x1740 `RBBM_STATUS` (0x80050110) and the
  ring registers `CP_RB_BASE` (the probe's ring), `CP_RB_RPTR` and `CP_RB_WPTR` (both 0x11E). Its CP debug words held
  the `EVENT_WRITE_SHD` packet's value and address, so the command processor was stuck in that fence.

A third run synced after zeroing the ring, before `VdInitializeRingBuffer`, and submitted libxenon's scratch fence with
a `MEM_WRITE` before the `EVENT_WRITE_SHD` (log: `results/2026-10-10-probe-ring-3/xsdk.log`).

- **The kernel doesn't put packets in the title's ring.** Every dword before the write pointer it left (229, up from
  0xC7) read back as zero.
- **The command processor stalls during the kernel's set-up.** The GPU was busy right after
  `VdEnableRingBufferRPtrWriteBack`, with 0x07D8 bit 29 set. None of the fences landed (`MEM_WRITE`, scratch write-back,
  `EVENT_WRITE_SHD`), though the read pointer followed the write pointer each time. D3D's hang report again showed the
  `EVENT_WRITE_SHD` packet in the command processor's debug words.

So without `VdInitializeEngines` the kernel's ring set-up stalls the command processor (two runs of three; in the
first, one `MEM_WRITE` landed). The probe now sets the ring up through the registers as libxenon does.

A fourth run set the ring up through the registers, as libxenon does, and didn't call the kernel's ring functions (log:
`results/2026-10-10-probe-ring-4/xsdk.log`, from xbWatson's copy).

- **Register set-up ends the same way as the kernel's.** Afterwards 0x0700, 0x0704 and 0x070C held the probe's values,
  but the read and write pointers were both 0xEB, though the probe had written 0 to the write pointer during the
  reset (0xD3 at start). The GPU was busy and 0x07D8 had bit 29 set. So the stall doesn't come from the kernel's ring
  functions. In all four runs something other than the title moved the write pointer during set-up, by 24 to 42
  dwords.
- **Phase A didn't land.** The read pointer write-back reached 13 at the new address, but neither the `MEM_WRITE` nor
  the scratch write-back landed within 1000 ms.
- **The probe crashed on its next packet, through its own fault.** GCC merged two 32-bit ring stores into one `std` at
  0xBFC40034, which isn't 8-byte aligned, and that raised an alignment exception in write-combined memory (xbWatson:
  EADDR 0x82010A04, `std r10,52(r28)`, r10 = 0xC0025800_00000014, the packet's header and event). Code that writes
  write-combined memory the GPU reads must use volatile 32-bit stores. The title carried on and submitted phase B.
- **XAM's D3D printed hang reports after the title exited.** The first had the probe's ring at both pointers 0x11 and
  the `EVENT_WRITE_SHD` packet's value and address in the CP debug words. The later ones (frames 18 to 20) had
  `CP_RB_BASE` 0, `CP_RB_RPTR` 0 and `CP_RB_WPTR` climbing from 0xFD to 0x11B, so that device kept submitting to a
  command processor that no longer fetched.

What moved the write pointer during set-up was XAM's D3D device, which keeps submitting while a title runs (next
section).

## XAM's D3D device shares the command processor (2026-10-10)

`tests/probe_cp.c` watched the ring registers for five seconds without writing to them, sampling about every 2 ms (log:
`results/2026-10-10-probe-cp-watch/xsdk.log`).

- **XAM holds a D3D device while a title runs, and the title has none.** `VdGlobalXamDevice` holds 0x7882FA00 and
  `VdGlobalDevice` holds 0. 0x7882FA00 is the device every GPU hang report in these runs names. Its non-snooped
  block, 0xE1B9D000, matches the ring base titles find, 0x01B9D000.
- **XAM submits to that ring every vblank.** The read and write pointers moved together by 12 dwords every 16 to 18 ms,
  wrapping at 0x400 (the control value's size, 9, is 2^9 8-byte units). The GPU was busy in 13 of 2507 samples, each
  time with 0x07D8 bit 29 set, so bit 29 comes with ordinary work and doesn't by itself mean a stall.
- **This explains the ring runs.** Once a title points 0x0700 at its own ring, XAM's next vblank submission writes
  XAM's write pointer to 0x0714. The command processor then runs whatever that index covers in the title's ring, and
  XAM's fences never complete. The 24 to 42 dwords the write pointer gained during set-up are two or three of those
  submissions.
- **XAM checks its device for hangs on its own thread.** In an xbWatson minidump of the break that follows each hang
  report, the breaking thread was created by XAM (start 0x816B9668), every frame is in `xamd.dll` or the kernel, r29
  and r31 hold 0x7882FA00, and r3 points at "The GPU is hung and can't be recovered without doing a cold boot."
- **Reading the ME microcode while the command processor runs hangs the GPU.** The GPU was healthy through the watch
  and afterwards, then the probe read the ME RAM through 0x07E4 and 0x07E8 (CRC-32 7A8E8248 again) and exited, and
  XAM broke on a hung GPU. That read was the probe's only write to the GPU.

So a title that wants the command processor has to stop XAM's device from submitting first. Xenia keeps a single
graphics interrupt callback that `VdSetGraphicsInterruptCallback` replaces and that vblank calls with source 0
(xenia:src/xenia/gpu/graphics_system.cc:234-272), so if XAM's vblank submissions run from that callback, a title that
sets its own stops them.

## A title's interrupt callback stops XAM's device (2026-10-10)

`tests/probe_interrupt.c` watched XAM's write pointer for 2 seconds at a time, without writing GPU registers (log:
`results/2026-10-10-probe-interrupt/xsdk.log`).

- **XAM submits from its graphics interrupt callback.** With XAM's callback in place the write pointer moved 121 times
  in 2000 ms (0xDF to 0x27F). After `VdSetGraphicsInterruptCallback` set the title's callback it didn't move at all,
  and it stayed at 0x28B after the title cleared the callback with `VdSetGraphicsInterruptCallback(0, 0)`. Nothing put
  XAM's callback back while the title ran.
- **The callback's vblank source fires twice per frame.** The title's callback ran 241 times in 2000 ms, every time
  with source 0, twice the rate XAM submitted at. Xenia calls it once per vsync (120 times in 2000 ms,
  xenia:src/xenia/gpu/graphics_system.cc:272).
- **The GPU stayed idle,** and the ring registers kept XAM's ring.
- **Taking XAM's callback breaks XAM once the title exits.** The console froze on the way back to Aurora. In an
  xbWatson minidump taken then, XAM's thread that checks its device holds "The GPU is hung and can't be recovered
  without doing a cold boot." in its live frames. Aurora's main thread is blocked early in its start-up: Aurora called
  into the kernel (return address 0x800C7E4C), the kernel called into XAM (`xamd.dll`+0xDC278), and XAM is waiting.
  The probe never wrote a GPU register and left the GPU idle, so XAM's device seems to need its callback to finish its
  work, and nothing gave it back. A title can't give it back itself, since no export returns the current callback.

So a title's callback stops XAM's device, but XAM can't recover afterwards. Aurora's start-up waits on XAM through the
kernel, which suggests D3D titles take the GPU from XAM through a kernel call. Xenia lists exports that fit
(`VdRegisterXamGraphicsNotification`, `VdCallGraphicsNotificationRoutines`, `VdBlockUntilGUIIdle`) but stubs them
(xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:446-447, 476; xboxkrnl_video.cc:310-319).

## Aurora takes the GPU from XAM in VdInitializeEngines (2026-10-10)

`tests/probe_exports.c` asked the kernel for every export's address with `XexGetProcedureAddress` (921 of 1023
ordinals resolve) and placed the kernel return addresses of Aurora's blocked start-up among them (log:
`results/2026-10-10-probe-exports/xsdk.log`).

- **0x800C7E4C is in `VdInitializeEngines`:** 0x34 past its start at 0x800C7E18, and below `VdShutdownEngines` at
  0x800C7F60.
- **0x800BD7E8 is in `VdCallGraphicsNotificationRoutines`:** 0x40 past its start at 0x800BD7A8, and below
  `VdRegisterGraphicsNotification` at 0x800BD840.
- **0x800C7DFC is in an unexported function** just below `VdInitializeEngines`.

So Aurora's D3D called `VdInitializeEngines`, which ran the graphics notification routines, and the one in `xamd.dll`
waited for XAM's device to stop. That device couldn't stop without its callback. A title takes the GPU from XAM in
the same call that loads the microcode.

## VdInitializeEngines without microcode (2026-10-10)

`tests/probe_engines.c` called `VdInitializeEngines(0x4F810000, callback, 0, 0, 0)`, with no microcode, and left XAM's
interrupt callback alone (log: `results/2026-10-10-probe-engines/xsdk.log`).

- **The kernel takes the GPU from XAM without microcode.** The call returned 1 and never called its callback.
  Afterwards `VdGlobalXamDevice` held 0, XAM's write pointer stopped moving, the ring registers were cleared (base 0,
  control 0x08020000, both pointers 0), the GPU was idle, and 0x07D8 read 0x06007FFF (bits 25 and 26 newly set).
- **The kernel's ring set-up then works.** After `VdInitializeRingBuffer` and `VdEnableRingBufferRPtrWriteBack`,
  0x0700 and 0x070C held the probe's addresses, 0x0704 read 0x0002060C, both pointers were 0, and the GPU was idle.
- **The command processor runs the title's packets without the title supplying microcode.** The `MEM_WRITE` and
  libxenon's scratch fence both landed within 1 ms, and the read pointer write-back reached 13. Whether the kernel kept
  the running microcode or loaded its own doesn't show.
- **`EVENT_WRITE_SHD` with `CACHE_FLUSH_AND_INV_TS_EVENT` (20) still wedges the GPU.** The read pointer reached the
  packet's end, the value never landed, the GPU went busy and 0x07D8 gained bit 29. Xenia describes the packet as
  generating "a VS|PS_done event" (xenia:src/xenia/gpu/xenos.h:1595), so event 20, a cache flush, may be the wrong
  kind for it, or it may need pipeline state nothing has set yet.
- **`VdShutdownEngines` didn't return,** presumably held up by the wedged GPU, and the console stopped responding.
  xbWatson reported no exception.

So SCOPE.md section 3.2 has an answer in option 2's spirit: a title takes the command processor with
`VdInitializeEngines` and no microcode, and the kernel's ring calls work after it. Still open are giving the GPU back
cleanly and an `EVENT_WRITE` fence.

## Giving the GPU back with VdShutdownEngines (2026-10-10)

`tests/probe_engines.c` took the GPU as before, submitted three scratch fences, and gave the GPU back with
`VdShutdownEngines` (log: `results/2026-10-10-probe-engines-2/xsdk.log`, xbWatson's copy; the file on the console
stopped mid-line at 2050 bytes).

- **Fences repeat.** Each of the three fences' `MEM_WRITE` and scratch values landed within 1 ms, and the read pointer
  write-back reached 0x0D, 0x16 and 0x1F.
- **`VdShutdownEngines` gives the GPU back to XAM.** Afterwards 0x07D8 read 0x00007FFF again, the ring registers held
  XAM's ring again (base 0x01B9D000, control 0x00020309, write-back 0x01B9F03E), `VdGlobalXamDevice` held 0x7882FA00
  again, and XAM's write pointer moved 15 times in 250 ms.
- **`VdShutdownEngines` calls the callback passed to `VdInitializeEngines`,** once. The probe's callback takes no
  arguments.
- **The title exited normally and the console returned to Aurora.**

So a title drives the command processor with `VdInitializeEngines(0x4F810000, callback, context, 0, 0)`,
`VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, its packets, and `VdShutdownEngines` before it exits.

## An EVENT_WRITE fence (2026-10-10)

`tests/probe_engines.c` took the GPU and ran the three scratch fences as before, then tried `EVENT_WRITE_SHD` with
three events, stopping at the first that didn't land (log: `results/2026-10-10-probe-engines-3/xsdk.log`, xbWatson's
copy).

- **`EVENT_WRITE_SHD` with a shader-done event is a working fence.** With `VS_DONE_TS` (2) and with `PS_DONE_TS` (3)
  the value landed within 1 ms, and the read pointer write-back reached 0x23 and 0x27.
- **With `CACHE_FLUSH_TS` (4) it wedged the GPU,** like `CACHE_FLUSH_AND_INV_TS_EVENT` (20) before: the read pointer
  reached the end of the packet, the value never landed, the GPU stayed busy and 0x07D8 gained bit 29. The cache-flush
  events presumably need state for the caches they flush, which nothing had set up.
- **The probe then exited without `VdShutdownEngines`,** which would have waited on the wedged GPU.

So milestone 4's fence is `EVENT_WRITE_SHD` with `PS_DONE_TS` or `VS_DONE_TS`. The cache-flush events wait for the
render state of milestone 5.

## The command processor, milestone 4 (2026-10-10)

The test title passed all 34 checks on the console, the four `gpu:` checks among them, exited with status 0, and the
console returned to Aurora; the log is in `results/2026-10-10-milestone-4/xsdk.log`.

- **`xsdk_gpu_open` takes the GPU** with `VdInitializeEngines` and no microcode, and sets up the ring and the read
  pointer write-back with the kernel's calls.
- **A `NOP` and an `EVENT_WRITE_SHD` fence with `PS_DONE_TS` land.**
- **The ring wraps.** About 4,000 `NOP`s of 2 to 5 dwords, each followed by a fence, about 30,000 dwords in all, wrapped
  the 8192-dword ring nearly four times, with a `NOP` filling each wrap, and the last fence landed.
- **`xsdk_gpu_close` gives the GPU back.** It waited for a final fence, called `VdShutdownEngines`, and freed the ring
  and the write-back page with `MmFreePhysicalMemory(0, base)`, and the title then exited normally.

So milestone 4 is done, and SCOPE.md section 3.2 has its answer.

## VdSwap writes only a fence (2026-10-10)

`tests/probe_swap.c` took the GPU, filled a linear 1280x720 `k_8_8_8_8` front buffer from the CPU, and called `VdSwap`
on it with the arguments Xenia documents (log: `results/2026-10-10-probe-swap/xsdk.log`).

- **The display information differs from Xenia's.** `VdQueryVideoMode` gave 1280x720 at 59.94 Hz, hi-def and
  widescreen, but `VdGetCurrentDisplayInformation` gave a 640x480 front buffer for a 1280x720 display.
- **`VdGetSystemCommandBuffer` zeroes 0x94 bytes** of its first argument, as Xenia does, and wrote 0x471D to its second.
- **`VdSwap` wrote one fence into the 64 reserved dwords:** `EVENT_WRITE_SHD` with `PS_DONE_TS`, writing the fifth
  argument to the third (the "system writeback", with byte swap 2), then a `NOP` over the other 59. The fence landed.
  There were no display registers or front buffer address in the ring, so the flip happens outside it.
- **The screen stayed black** through the whole run.

## Logs cut off at the previous run's length (2026-10-10)

Four logs came back cut off mid-line, each at exactly the length of the run before it: 2050 bytes after a 2050-byte
log, 1454 after 1454, 962 after 962, and 1002 after 1002. Calling `NtFlushBuffersFile` after every line didn't change
that, so it was reverted. A shorter log always came back whole, which fits the FTP transfer reusing the old file's size
rather than the console's file system; that's unconfirmed. xbWatson's copy of a log is complete.

## The kernel doesn't flip on VdSwap (2026-10-10)

`tests/probe_swap.c` registered the swap fence's address with `VdSetSystemCommandBufferGpuIdentifierAddress`, passed
the value from `VdGetSystemCommandBuffer` as `VdSwap`'s fifth argument, and swapped first to a red 1280x720 buffer,
then to a blue 640x480 one, reading the display controller's registers (libxenon:drivers/xenos/xenos.h:26-44) along
the way (log: `results/2026-10-10-probe-swap-3/xsdk.log`).

- **At title start the display shows a 640x480 surface at address 0, scaled up.** `D1GRPH_ENABLE` 1,
  `D1GRPH_CONTROL` 2, `D1GRPH_PRIMARY_SURFACE_ADDRESS` 0, pitch and end 640x480, viewport size 640x480, scaler on.
  That matches the 640x480 front buffer `VdGetCurrentDisplayInformation` reports.
- **Neither swap changed the display.** Each `VdSwap` wrote the same fence, now with the value from
  `VdGetSystemCommandBuffer`, and it landed, but the surface address stayed 0 and the display information stayed
  640x480, so the screen stayed black. So the kernel doesn't flip on `VdSwap` and its fence alone.

## The display takes a title's surface but stays black (2026-10-10)

`tests/probe_display.c` kept the kernel's 640x480 mode and wrote only `D1GRPH_PRIMARY_SURFACE_ADDRESS`, inside
libxenon's update lock (`D1GRPH_UPDATE` 1, then 0; libxenon:drivers/xenos/xenos.c:366-379), pointing it at a buffer the
CPU filled (log: `results/2026-10-10-probe-display/xsdk.log`).

- **The display controller takes the address.** The register read back as the buffer's physical address, 0x1FB00000,
  with `D1GRPH_UPDATE` reading 4 just after the write and 0 by the end, and the old address went back the same way.
- **The screen stayed black** through red, green and blue in libxenon's pixel layout and the tiled stripe pattern. So
  something after the surface keeps the output black while a title runs.

## VdSetDisplayMode doesn't change the display (2026-10-10)

`tests/probe_display.c` called `VdSetDisplayMode(0x40000000)` after taking the GPU, then swapped with `VdSwap`, then set
the surface directly (log: `results/2026-10-10-probe-display-2/xsdk.log`, xbWatson's copy).

- **`VdSetDisplayMode` returned 0 and changed none of the display registers** or the 640x480 display information.
- **`VdSwap` again only landed its fence;** setting the surface directly again took, and the screen stayed black.
- **The LUT looks ordinary:** 0x64C0 to 0x64DC read 0, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0, black offsets 0 and white
  offsets at the maximum if they follow libxenon's `DC_LUTA_CONTROL` at 0x64C0. 0x6080 to 0x60BC, a range libxenon
  doesn't use, read all zeros.

## How XAM switches the display (2026-10-10)

`tests/probe_guide.c` left the GPU with XAM and logged every change to the display registers around libxenon's names
for 15 seconds while the user opened and closed the Guide (log: `results/2026-10-10-probe-guide/xsdk.log`).

- **XAM switched the display to its own surface twice and back,** at 1021 and 6701 ms, back at 4890 and 9654 ms. Each
  time it changed only these registers, all of them back afterwards:
  - 0x6110 `D1GRPH_PRIMARY_SURFACE_ADDRESS`: 0 to 0x01C10000;
  - 0x6120 `D1GRPH_PITCH` and 0x6134 `D1GRPH_X_END`: 640 to 864;
  - 0x6584 `AVIVO_D1MODE_VIEWPORT_SIZE`: 640x480 to 852x480;
  - 0x65B4: 0x00800000 to 0x00AA6660, the ratio 852/640 if 0x00800000 is 1.0, and 0x65B8: 0x0005C000 to 0x0005D600;
  - 0x65E8: 0x9F to 0xD4, the viewport width / 4 - 1, as libxenon writes it (libxenon:drivers/xenos/xenos.c:390);
  - 0x60EC, libxenon's `D1CRTC_DOUBLE_BUFFER_CONTROL`: 1 in the sample where the new values appeared, 0 by the next.
- **Nothing else changed:** no blanking or LUT register. Several registers count or toggle every frame (0x6024, 0x6054,
  0x6058 to 0x6068, 0x6530, 0x6554, 0x6578, 0x657C), and 0x6144 `D1GRPH_UPDATE` read 2 for a sample every so often,
  with 0x6534 and 0x6544 going to 0x1000 and 0x10 at the same moments.

So XAM shows its surface the way the probes tried, plus 0x60EC, which the probes never wrote.

## XAM's display registers don't show a title's buffer either (2026-10-10)

`tests/probe_display.c` took the GPU and applied a surface the way XAM did: the registers, then 0x60EC = 1. First only
the address of a red buffer in the kernel's 640x480 mode, then XAM's whole set (852x480 at pitch 864, its scaler
values) on a blue buffer, then the originals (log: `results/2026-10-10-probe-display-3/xsdk.log`).

- **Every register took its value, and 0x60EC read 0 again straight away** each time, where XAM's read 1 for a sample.
- **The screen stayed black** throughout. Every colour the probes have shown had a zero low byte, which is the alpha if
  the display reads pixels little-endian, as libxenon's console suggests (libxenon:drivers/console/console.c:67-70).

The Guide did appear on screen during the run that recorded XAM's registers, so the output isn't muted while a title
runs: XAM's surface shows through the same registers that leave the title's buffers black.

## A title can't move the display while XAM has the GPU (2026-10-10)

`tests/probe_display.c` showed opaque white (0xFFFFFFFF) from a buffer high in memory (physical 0x1FB20000) and one low
(0x03ED0000), both before taking the GPU, then the high one after taking it, then red with an opaque alpha (log:
`results/2026-10-10-probe-display-4/xsdk.log`, cut off after the second step).

- **Before the title takes the GPU, its surface writes don't stick.** Both times `D1GRPH_PRIMARY_SURFACE_ADDRESS` read
  0 straight after the write and 0x60EC. After taking the GPU, earlier runs showed the address sticking.
- **Nothing showed on screen** at any step, opaque white from either buffer included.

So the display is held on the kernel's placeholder while XAM has the GPU, and shows nothing the title gives it after.

## What XAM writes to show the Guide (2026-10-10)

`tests/probe_guide.c` watched wider register blocks while the Guide opened and closed, then snapshotted them around
`VdInitializeEngines` and `VdShutdownEngines` (log: `results/2026-10-10-probe-guide-2/xsdk.log`).

- **XAM also writes its surface to 0x2840 and 0x2844,** alongside D1GRPH: 0x2840 took 0x01C10000, the same address as
  0x6110, and 0x2844 took 864, the same as the pitch. libxenon writes its framebuffer address and width there too
  (libxenon:drivers/xenos/xenos.c:384-386). The earlier probes never wrote them.
- **XAM loads the LUT as the Guide appears.** `DC_LUT_RW_INDEX` (0x6488) counted up over about 200 ms while 0x648C,
  0x6490 and 0x6494 took a rising ramp (0x6494 went 0x00100401, 0x00401004, 0x00601806, ...), so the Guide fades in
  through the LUT. At start `DC_LUT_WRITE_EN_MASK` read 7 and `DC_LUT_AUTOFILL` 2.
- **0x28BC, 0x28C0 and 0x28C4** went from 0x00010000, 0x01B8D000 and 0x01000000 to 0, 0x1000 and 0x10000 while the Guide
  showed, and back after.
- **`VdInitializeEngines` changed none of these registers.** `VdShutdownEngines` handed the display back to XAM, which
  set a 1280x720 surface at 0x1FC59000 in both 0x2840 and 0x6110, with 1280 in 0x2844, 0x6120 and 0x6134 and 720 in
  0x6138.

So a surface shows when 0x2840 and 0x2844 match D1GRPH, and the LUT may be holding the screen black in between.

## A title's buffer on screen (2026-10-10)

`tests/probe_display.c` took the GPU, then set the surface address and pitch in both 0x2840/0x2844 and D1GRPH and set
0x60EC, keeping the kernel's 640x480 mode, and showed a buffer the CPU filled. The user reported what the screen showed.

- **White didn't show with the surface set alone.**
- **White showed once the LUT was filled** with libxenon's autofill: `DC_LUTA_CONTROL` 0, `DC_LUT_RW_INDEX` 0,
  `DC_LUT_RW_MODE` 0, `DC_LUT_WRITE_EN_MASK` 7, `DC_LUT_AUTOFILL` 1, then waiting for its bit 1
  (libxenon:drivers/xenos/xenos.c:419-424). So the LUT holds the screen black while a title runs, and every earlier
  test was black because of it.
- **Pixels are 0xAARRGGBB stored little-endian.** The words 0x0000FFFF, 0x00FF00FF and 0xFF0000FF, written by the
  CPU, showed red, green and blue.
- **The display reads libxenon's 32x32 tile layout** (libxenon:drivers/console/console.c:60-61): red and blue stripes
  drawn with it showed as stripes, with the white diagonal in place.

Whether 0x2840 and 0x2844 are needed as well as D1GRPH isn't settled, since the surface was set in both throughout.

## A first resolve runs but copies only parts (2026-10-10)

`tests/probe_resolve.c` showed a CPU-filled magenta 640x480 front buffer, then resolved four times in libxenon's way,
each clearing eDRAM to the next colour (white, red, green, black) while copying the previous one into the front buffer
(log: `results/2026-10-10-probe-resolve/xsdk.log`).

- **Every stage finished:** state and the inline vertex shader, the rectangle draw in copy mode, `EVENT_WRITE`
  `CACHE_FLUSH` and the coherency wait all let their fences land. So with render-target state set, the cache flush
  doesn't wedge the GPU.
- **The copies landed only in parts.** Pixels 0 and 100000 stayed magenta through the first two resolves and read 0
  after the third and fourth. The user saw no white, red or blue, and at one point magenta and green in stripes.
- **The console froze when Aurora came back.** The probe left `RB_MODECONTROL` at 6 (copy) and had loaded its shader at
  instruction address 0, either of which could have upset XAM's device.

## The resolve's tile map (2026-10-10)

`tests/probe_resolve.c` repeated the four resolves and mapped the front buffer, one character per 4 KB tile, after
each one and half a second later (log: `results/2026-10-10-probe-resolve-2/xsdk.log`, xbWatson's copy).

- **The first two resolves wrote nothing:** every tile stayed magenta, though all their stages' fences landed.
- **The third wrote zero over the whole frame,** not the white or red the earlier clears should have left in eDRAM, so
  when a copy does land its rectangle covers the screen. After the fourth every tile still read zero.
- **Nothing changed in the half second after each map,** so the writes don't arrive late in time.

## Only the third resolve writes (2026-10-10)

`tests/probe_resolve.c` refilled the front buffer with magenta before each of four resolves, cleared white, white, green
and green, and mapped the buffer after each resolve and again after a second cache flush and coherency wait (log:
`results/2026-10-10-probe-resolve-3/xsdk.log`, xbWatson's copy).

- **The first, second and fourth resolves wrote nothing:** every tile stayed magenta, though all their fences landed.
- **The third changed every tile, but not to one value:** each tile held more than one value, and none started with a
  value outside the probe's colours and zero. The map can't tell part-white, part-magenta tiles from others.
- **The second flush changed nothing,** so the missing writes aren't held in a cache.
- **The third resolve was also the only one to write in the previous run,** whose holds and stages differed, so the
  pattern follows the resolve's position in the sequence, not the time or the ring position.

## Every third resolve writes (2026-10-10)

`tests/probe_resolve.c` flushed the fetch constants after setting them, as libxenon does
(libxenon:drivers/xenos/xe.c:1146), then ran eight resolves, clearing white, white, green, green, blue, blue, white and
white, with the front buffer refilled with magenta before each (log: `results/2026-10-10-probe-resolve-4/xsdk.log`).

- **Only the third and sixth resolves wrote.** The third wrote 0 to all 307200 pixels, the sixth 0x00FF00FF (green) to
  all of them. Every other resolve left every pixel magenta, though all fences landed.
- **A skipped resolve skips its clear too.** The sixth copied the green the third cleared to, not the blue the fifth
  should have left, so the skipped resolves did nothing in eDRAM.
- **The green clear value 0xFF00FF00 landed as the word 0x00FF00FF,** the green the display shows.
- **The fetch constant flush didn't change the pattern.**
- **The console still froze on the way back to Aurora,** though the probe set `RB_MODECONTROL` back to 4 first.

## libxenon's set-up makes the resolves work (2026-10-10)

`tests/probe_resolve.c` took the GPU, sent libxenon's set-up (`tests/libxenon_gpu.h`, from
libxenon:drivers/xenos/xe.c:398-568) in fenced stages, resolved eight times as libxenon does, and wrote back the
registers it had changed before giving the GPU up (log: `results/2026-10-10-probe-resolve-5/xsdk.log`, cut off during
the second resolve). The user reported that the run went perfectly and the console returned to Aurora.

- **Every set-up stage's fence landed,** `ME_INIT` included, with the console's own microcode.
- **The first two resolves wrote,** where before only every third did: the first copied zero over all 307200 pixels,
  and the second the white the first cleared to.
- **The kernel leaves the shader unit set up differently from libxenon.** Read after taking the GPU, then after the
  set-up: `SQ_GPR_MANAGEMENT` 0x00040400, then libxenon's 0x00070100; `SQ_INST_STORE_MANAGMENT` 0x00000800, then
  0x00010800; `PA_CL_ENHANCE` 7, then 3. `SQ_FLOW_CONTROL` read 0x04007000 both times, though the set-up wrote
  0x04000000. `UNKNOWN_0A02` to `0A05`, `UNKNOWN_0E42` and `RB_BC_CONTROL` already held libxenon's values.
- **Drawing registers don't read back what a title writes.** From 0x2000 up, each read the same before and after the
  set-up, even where it wrote another value: the window scissor read 16x16 after the set-up set 1024x720. The values
  look like XAM's last resolve: a 1280-pixel surface and a 1280x720 copy to 0x1C0F2000.
- **The console didn't freeze on the way back to Aurora.** The probe wrote back what it read, which for drawing
  registers is that XAM-like state.

Which part of the set-up makes every resolve write, and which write-back stops the freeze, isn't isolated.

## The SDK resolves every frame; the display's size is the viewport (2026-10-10)

The test title ran `tests/test_present.c` after `tests/test_gpu.c` had opened and closed the GPU: it opened the GPU
again through the SDK, which now sends libxenon's set-up, and resolved 240 frames into two front buffers in turn,
fading the clear colour (log: `results/2026-10-10-milestone-5/xsdk.log`).

- **Every resolve landed:** all 240 fences, and every frame after the first held the colour the resolve before it
  cleared to, at its first, middle and last pixel. So the set-up works from `xsdk_gpu_open`, also on a second open.
- **The display check failed.** `xsdk_display_open` took the size from `D1GRPH_PITCH` and `D1GRPH_Y_END`, which XAM
  sets to 1280x720 when `VdShutdownEngines` hands the GPU back ("What XAM writes to show the Guide"). Through that
  hand-back `AVIVO_D1MODE_VIEWPORT_SIZE` (0x6584) stayed 0x028001E0, 640 wide in its high half and 480 high
  (`results/2026-10-10-probe-guide-2/xsdk.log`), so the display still scaled up a 640x480 surface. With the size
  wrong, the test showed nothing.
- **The other 39 checks passed.**

## A cleared screen, milestone 5 (2026-10-10)

The test title passed all 41 checks on the console and exited with status 0; the log is in
`results/2026-10-10-milestone-5-2/xsdk.log`.

- **`xsdk_gpu_open` sets the GPU up as libxenon does,** after taking it, on the title's second open as well as its
  first, and `xsdk_gpu_close` gives it back each time.
- **`xsdk_display_open` reports the viewport's 640x480,** though the earlier close had left XAM's surface registers
  at 1280x720, and fills the LUT.
- **Every frame clears, resolves and presents.** Over 240 frames the clear colour faded between magenta and green;
  each frame resolved into one of two front buffers in turn, held the colour the resolve before it cleared to at its
  first, middle and last pixel, and was shown with `xsdk_display_show`, whose 0x60EC write the display took every
  time.

The user didn't report what the screen showed in this run; earlier runs showed CPU-filled buffers through the same
registers ("A title's buffer on screen").

## A resolve keeps colours; the clear colour is packed red-low (2026-10-10)

`tests/probe_resolve.c` cleared eDRAM through the SDK with `RB_COLOR_CLEAR` 0xFFFF0000, resolved it into a magenta
front buffer and showed it (log: `results/2026-10-10-probe-resolve-6/xsdk.log`).

- **Every pixel held 0xFF0000FF, and the user saw blue,** as the CPU's 0xFF0000FF shows ("A title's buffer on screen").
- **Xenia's model gives the same:** a `k_8_8_8_8` target packs red into the low byte (xenia:
  src/xenia/gpu/shaders/pixel_formats.xesli:113-116, 289-292), the clear writes `RB_COLOR_CLEAR` as that packed value,
  and the copy's swap bit, which libxenon sets, swaps red and blue on the way out (shaders/resolve.xesli:632-635). By
  that model 0xFFFF0000 is a blue clear, and the green run's 0x00FF00FF comes out too.

So the copy and the display carry a colour through unchanged, and only the clear value needs red and blue swapped
from 0xAARRGGBB. What a shader draws isn't covered yet.

## The packed clear shows red (2026-10-10)

With `xsdk_gpu_resolve` swapping red and blue in the clear colour, the same probe's red clear stored 0x0000FFFF in all
307200 pixels, the word the display shows as red (log: `results/2026-10-10-probe-resolve-7/xsdk.log`). The user didn't
report the screen in this run; the log names the colour the stored word shows.

## Milestone 5 with a red and blue fade (2026-10-10)

The test title, with the present test fading between red and blue, passed all 41 checks, exited with status 0 and
returned to Aurora (log: `results/2026-10-10-milestone-5-3/xsdk.log`). Every frame held the colour cleared before it,
red and blue in the right places, so `xsdk_gpu_resolve`'s packing of the clear colour holds across 240 frames.

## The first triangle draws nothing (2026-10-10)

The test title ran `tests/test_triangle.c`: shaders from `tools/gen_triangle_shaders.py` loaded with `IM_LOAD`, its
size given in dwords, the vertex fetches patched, and 120 frames each drawing a triangle with `DRAW_INDX_2` and
resolving it into a front buffer (log: `results/2026-10-10-milestone-6/xsdk.log`, xbWatson's copy).

- **Every frame's fence landed, but nothing was drawn:** the pixels inside the three corners held the clear colour in
  every frame checked, and the last frame had no pixel other than the clear colour. The same commands draw the triangle
  in Xenia, all 86160 pixels of it.
- **The log ended after the triangle test's last check,** before the GPU was closed and before the test count and exit
  lines, so the title seems to have stopped there.
- **The vertex shader matches xost's passthrough,** which xost has drawn on hardware through libxenon: the same control
  flow, `SQ_PROGRAM_CNTL` and fetch encoding, apart from its registers and the fields the patch rewrites. libxenon
  gives `IM_LOAD` the shader's size in bytes (libxenon:drivers/xenos/xe.c:1210-1215, 1383), where the SDK gave dwords.

## The triangle doesn't draw however its shaders load (2026-10-10)

`tests/probe_triangle.c` drew the triangle four ways into the same front buffer, each after a resolve that cleared eDRAM
to grey, and counted the pixels other than grey (log: `results/2026-10-10-probe-triangle/xsdk.log`).

- **With a fence before the resolve, every pixel came out grey,** whether `IM_LOAD` was given the size in bytes or the
  shaders were loaded inline with `IM_LOAD_IMMEDIATE`, as the resolve loads its own. So the draw writes nothing, and
  how the shaders are loaded isn't why.
- **Without that fence, every pixel held zero,** with `IM_LOAD`'s size in dwords and in bytes. The buffer was never
  filled by the CPU, so the resolve after the draw most likely wrote nothing.
- **The patched vertex fetches read back as written.**
- **The console froze after returning to Aurora,** as it did before the probe gave back the registers it changed.

## No single draw setting makes the triangle draw (2026-10-10)

`tests/probe_triangle.c` drew the triangle six ways, each from an idle GPU with eDRAM cleared to grey, its shaders
loaded inline, a fence after the draw, and a magenta front buffer (log: `results/2026-10-10-probe-triangle-2/xsdk.log`).

- **None drew a pixel:** not as the SDK draws, nor with z 0.5, clipping off, `DRAW_INDX` with a viz query token,
  vertex fetch constant 0, or screen-space corners with no viewport transform or clipping, as the resolve draws.
- **After three of them the resolve wrote nothing,** leaving every pixel magenta, though a fence came between the draw
  and the resolve: z 0.5, `DRAW_INDX` and fetch constant 0. After the other three it wrote grey.

## libxenon's frame draws the triangle (2026-10-10)

`tests/probe_triangle.c` drew the triangle with the frame that xost's libxenon triangle app sends, as libxenon's `xe.c`
builds it on the host, then with one of its differences from the SDK's draw undone at a time, then as the SDK draws
(log: `results/2026-10-10-probe-triangle-3/xsdk.log`).

- **libxenon's frame drew the triangle:** 86160 pixels, as in Xenia, red at the top. So the kernel's command processor
  microcode, the SDK's memory and the generated shaders can draw it.
- **It still drew with each difference undone:** `IM_LOAD`'s size in dwords, no depth test, `DRAW_INDX_2`, and the ALU
  or the bool and loop constants left alone.
- **The SDK's draw, run last, drew nothing, and the resolve after it wrote nothing,** though every fence landed. The
  earlier draws had zeroed the constants and pointed vertex fetch constant 95 at the same vertices, so what was left to
  differ was those five settings together and how the SDK sets the fetch constant: its two dwords alone from `0x48BE`,
  its size in dwords, 0 in `SHADER_CONSTANT_FLUSH_FETCH_1` where libxenon writes 0x00025000, its own vertex cache
  invalidate, and the order of its packets.
- **The console froze after returning to Aurora.**

## Fetch constants go in whole groups (2026-10-10)

`tests/probe_triangle.c` drew the triangle the SDK's way with one of libxenon's ways of setting the vertex fetch
constant at a time, each followed by libxenon's whole frame as a check (log:
`results/2026-10-10-probe-triangle-4/xsdk.log`).

- **The SDK's draw drew the triangle, all 86160 pixels, when fetch constant 95 went with the two before it,** the six
  dwords from `0x48BA`, or with every fetch constant. libxenon writes fetch constants in such groups of three
  (libxenon:drivers/xenos/xe.c:1334-1355), and the resolve writes fetch constant 0 with its group.
- **Written alone, its two dwords from `0x48BE` drew nothing:** with its data on 16 bytes in the ring, with 0x00025000
  in `SHADER_CONSTANT_FLUSH_FETCH_1`, with its size in bytes, without the vertex cache invalidate, with the target set
  after it, and as the SDK drew.
- **Lone writes broke drawing in threes.** After the first, no resolve wrote anything and libxenon's frame drew nothing,
  until the third, whose own triangle didn't draw but whose resolve wrote; libxenon's frame drew again after it. It
  went the same way twice, and the whole-group writes in between, libxenon's frame with the SDK's five settings among
  them, didn't change it. So the GPU seems to take fetch constants six dwords at a time, whatever their address.
- **That fits the earlier runs:** the previous one's single lone write, last, left the resolve writing nothing and
  Aurora frozen, and in the one before (`results/2026-10-10-probe-triangle-2/xsdk.log`) it fits five of the six
  resolves; the first wrote though it came after a lone write.

## The triangle draws (2026-10-10)

The test title ran `tests/test_triangle.c` with `xsdk_gpu_set_vertices` writing the fetch constant's whole group (log:
`results/2026-10-10-milestone-6-2/xsdk.log`, xbWatson's copy).

- **Every triangle check passed:** the last frame drew 86160 pixels, as in Xenia, every frame had red, green and blue
  corners on the clear colour, and the GPU closed.
- **The present test failed, though it passed in the first triangle run and nothing it runs has changed since.** In the
  four frames logged, the first pixel held the colour the resolve before had cleared to, so the middle or the last pixel
  didn't; the test logged only the first.

## Resolves without draws miss the last pixel (2026-10-10)

The test title ran again, the present test now logging the three pixels it checks (log:
`results/2026-10-10-milestone-6-3/xsdk.log`, xbWatson's copy).

- **The triangle drew and passed again;** the user saw it, and the console went back to Aurora without freezing.
- **Every present frame missed its last pixel,** (639, 479), which still held the zero the buffer was allocated with,
  while the first and middle pixels held the colour cleared before. The present test passed in the runs before the
  last two.
- **The triangle's resolves in the same run wrote that pixel:** unwritten, it would have counted as drawn, and the last
  frame drew 86160 pixels, as in Xenia. The triangle sets its draw state before resolving, where the present test only
  resolves.
- **A missing last row and column is what the other pixel centre would give.** The resolve's corners sit half a pixel
  up and left (libxenon:drivers/xenos/xe.c:1074-1081); Xenia adds that half pixel back for Direct3D centres, which the
  set-up selects, and without it coverage stops one pixel short of the right and bottom
  (xenia:src/xenia/gpu/draw_util.cc:837-867).

## A resolve needs multisampling off (2026-10-10)

`tests/probe_resolve.c` read the drawing registers XAM left, resolved as the present test does, then after setting
`PA_SC_AA_CONFIG` to 0, then after the rest of the triangle's target state (log:
`results/2026-10-10-probe-resolve-8/xsdk.log`).

- **XAM had left multisampling set up:** `PA_SC_AA_CONFIG` 0x0000C003, next to libxenon's 4x value 0x0000C004
  (libxenon:drivers/xenos/xe.c:1738-1740), and `RB_SURFACE_INFO` with 4x samples, which the resolve overwrites.
- **With it, each resolve wrote half the pixels:** 153600 were left unwritten, the first at (8, 0), with 320 of the last
  row's 640 and all of the last column, as if every other 8-pixel column were skipped. The present test's first and
  middle pixels, both at x = 0, fell in columns that were written.
- **With `PA_SC_AA_CONFIG` 0, every resolve wrote every pixel,** and after the triangle's target state too, which sets
  it to 0 with the rest.

The resolve now sets `PA_SC_AA_CONFIG` to 0, and the SDK gives XAM's value back when it closes the GPU.

## A triangle, milestone 6 (2026-10-10)

The test title passed all 51 checks on the console, exited with status 0 and went back to Aurora without freezing; the
log is in `results/2026-10-10-milestone-6-4/xsdk.log`.

- **xost-encoded shaders, loaded with `IM_LOAD`, drew a coloured triangle from a vertex buffer.**
  `xsdk_shader_load` took the containers from `tools/gen_triangle_shaders.py`, `xsdk_shader_set_vertex_layout`
  patched the vertex fetches for a float4 position and colour, and `xsdk_gpu_set_vertices` wrote vertex fetch constant
  95 with its group. Each of 120 frames had red, green and blue corners on the clear colour, and the last drew 86160
  pixels, as in Xenia. The user saw the triangle in the run before.
- **The present test passed again** with the resolve setting `PA_SC_AA_CONFIG` to 0: every frame held the colour
  cleared before it.

## A textured quad, milestone 7 (2026-10-10)

The test title passed all 66 checks on the console and exited with status 0; the log is in
`results/2026-10-10-milestone-7/xsdk.log`, and `capture.png` beside it is the user's capture of one frame.

- **A texture the SDK uploaded sampled correctly, linear and then tiled.** A 160x96 ARGB texture whose texels all
  differ, drawn one texel to a pixel at (64, 48), held every texel in place in the last of 60 frames each way, with the
  pixels around it the clear colour, and five texels checked in every frame. So the fetch constant, the ARGB swizzle,
  the 256-byte linear pitch, `xsdk_tiled_2d` and the float2 texture coordinates all hold on the console.
- **The capture shows the texture as written:** navy at the top left, red rising to the right and green downwards, and
  blue wrapping from 255 to 128 along x + y = 128, from texel 128 on the top edge to about texel 33 on the bottom.

## Linear and tiled textures side by side (2026-10-10)

The texture test now draws both textures in every frame, the tiled one with red and green swapped and its blue bands
slanted the other way, so a capture shows both and a quad sampling the other texture fails. The test title passed all
64 checks and exited with status 0 (log: `results/2026-10-10-milestone-7-2/xsdk.log`, the user's capture beside it).

- **Each quad held its own texture, every texel in place:** no wrong pixel in either quad or its border in the last of
  120 frames, with the texture rebound to slot 0 between the two draws of each frame.
- **The capture shows both as written:** on the left the linear texture as before; on the right green rising to the
  right and red downwards, a blue seam from the top left corner to texel (95, 95) on the bottom edge, and a teal corner
  cut off from texel 128 on the top edge to row 31 on the right.

## Render to texture, milestone 8 (2026-10-10)

The test title passed all 77 checks on the console and exited with status 0; the log is in
`results/2026-10-10-milestone-8/xsdk.log`, with the user's captures of each half beside it.

- **A scene resolved into a texture that a second pass sampled.** Each of 120 frames drew the source texture mirrored
  over a 160x96 render target, resolved it into that tiled texture with `xsdk_gpu_resolve_texture`, then drew the
  source and the render target side by side on screen. The rendered quad held the mirrored source in every texel in
  the last frame of each half, and its sampled texels in every frame, through the switch from mirroring left to right
  to top to bottom at frame 60, so no frame sampled a stale texture. The render target started magenta.
- **The resolve's 8-in-32 byte order leaves ARGB words,** as for libxenon's ARGB surfaces
  (libxenon:drivers/xenos/xe.c:1104-1108): sampled with the same fetch constant as an uploaded texture, every colour
  came out right.
- **Both resolve sizes in each frame came out right** with a rectangle kept for each; this run didn't time whether the
  CPU still waited.
- **The captures show both halves as written:** first the right quad mirrored left to right, magenta at its top left
  and its seam from about texel 32 at the top to 127 at the bottom; then mirrored top to bottom, navy at its bottom
  left and its seam from texel 33 at the top to 128 at the bottom.

## The vblank callback, flips and giving the callback back (2026-10-10)

`tests/probe_vblank.c` took the GPU, set its own graphics interrupt callback, recorded each call for about a second
with the time base and the display registers that change each frame, flipped between two greys ten times, then
cleared the callback, gave the GPU back and watched XAM's write pointer (log:
`results/2026-10-10-probe-vblank/xsdk.log`).

- **The callback comes twice a frame, 930 us apart, always with source 0.** The first of each pair read 0x6530 452,
  0x6534 0 and 0x6544 0x00110000; the second read 0x6530 480, 0x6534 0x1001 and 0x6544 0x11. Each pair started 16624
  us after the last. Exactly half the calls, 67 of 134, had 0x6534's bit 12 set, which libxenon reads as vblank
  (libxenon:drivers/xenos/xe.c:2141-2145). 0x6530 looks like the current line: 480 is the mode's height, where vblank
  starts, so the first call comes 28 lines before vblank and the second as it starts.
- **Nothing shows a flip waiting for vblank.** After each address write, 0x60EC read 0 again at once, at any point in
  the frame, and `D1GRPH_UPDATE` read 0, so nothing held the flip for vblank. The user saw the greys alternate. So a
  flip that mustn't tear has to be written during vblank.
- **Giving the GPU back with the title's callback cleared lets XAM run again.** After `VdSetGraphicsInterruptCallback(0,
  0)` and `xsdk_gpu_close`, XAM's write pointer moved 15 times in 250 ms, as after a run that never set a callback
  ("Giving the GPU back with VdShutdownEngines").

## The pacing modes pace, but small vertex invalidates don't take (2026-10-10)

The test title ran `tests/test_pacing.c`: each of four present modes for 120 frames, a moving bar its own colour per
mode, the GPU open throughout and the bar's vertex buffer allocated anew for each mode (logs:
`results/2026-10-10-milestone-9/xsdk.log`, then `results/2026-10-10-milestone-9-2/xsdk.log` with the misses logged).

- **Every mode paced exactly.** Three and two buffers flipped 120 frames over 120 vblanks, and at a swap interval of 2
  over 240, with no vblank missed and no flip late; the longest wait for a buffer was 17949 us at interval 1 and 33909
  us at 2. Every earlier test passed too, now that the SDK's waits return an error when they time out.
- **After the first mode, the bar came out in the previous mode's colour.** The yellow mode's second frame held white
  (0xFFFFFFFF), the cyan mode's yellow and the magenta mode's cyan, and the first frame of each a blend between the two;
  40 of each mode's 118 checked frames were wrong. The new vertex buffer most likely sat in the memory the last mode's
  had used, and the vertex cache still held it, though `xsdk_gpu_set_vertices` invalidated the vertex cache over the 192
  bytes each draw read. The first mode followed `xsdk_gpu_open`'s invalidate of all memory and came out right.
- **libxenon never invalidates less than a page:** it rounds a lock up to 4096 bytes and adds a page to its vertex
  pool's range (libxenon:drivers/xenos/xe.c:766, 1871). So the SDK now rounds every invalidate out to whole 4 KB pages.

## Whole-page invalidates keep vertices fresh (2026-10-10)

With `xsdk_gpu_put_coherency` rounding every range out to whole 4 KB pages, the test title passed all 103 checks and
exited with status 0 (log: `results/2026-10-10-milestone-9-3/xsdk.log`).

- **Every pacing mode drew every frame's own bar:** no reused buffer held the wrong frame in any of the four modes,
  where the yellow, cyan and magenta modes had 40 each before.
- **The pacing held as before:** 120 flips over 120 vblanks at interval 1 and over 240 at interval 2, none missed or
  late, in all four modes.

## Ten minutes of paced frames, milestone 9 (2026-10-10)

`tests/pacing.c` presented the moving bar for 10 minutes with three front buffers and a flip every vblank, checking
each frame's bar as its buffer came back (log: `results/2026-10-10-milestone-9-4/xsdk.log`).

- **Every frame flipped on its own vblank for the whole run:** 36093 frames, 36093 flips over 36094 vblanks, no vblank
  missed and no flip late, the minute-by-minute counts rising by 3609 each minute. The longest wait for a buffer was
  17950 us, one vblank, as the pacing throttles the title.
- **Every frame after the first held its bar.** Frame 0 didn't, and the title didn't log its pixels. Its background is
  whatever eDRAM held before the first resolve cleared it, which is why the other tests skip frame 0; the pacing checks
  now skip it too and log the pixels of any miss.

## Frame pacing, milestone 9 (2026-10-10)

`tests/pacing.c` ran its 10 minutes again with frame 0 left out of the bar check, and passed all 8 checks with status 0
(log: `results/2026-10-10-milestone-9-5/xsdk.log`).

- **Frames were paced to vsync through the interrupt callback and fences for 10 minutes:** 36094 frames, 36094 flips
  over 36095 vblanks, a flip every vblank, with three front buffers, so two frames in flight behind the one shown.
- **No stalls and no tearing, by the run's own counts:** no vblank missed, so no frame shown twice, and no flip still
  being written once the scan had left vblank.
- **Every frame from frame 1 on held its own bar** when its buffer came back, 3609 more frames each minute, and the
  longest wait for a buffer stayed at 17950 us, one vblank.

The console went back to Aurora normally afterwards, the user reported.

## The console outputs 1920 pixels wide; a 1280x720 surface shows 1:1 (2026-10-10)

`tests/probe_720p.c` showed a 1280x720 image in the kernel's 640x480 viewport, then with libxenon's scaler settings for
a mode shown 1:1 (libxenon:drivers/xenos/xenos.c:381-417), logging the display's registers, and put them back (log:
`results/2026-10-10-probe-720p/xsdk.log`, with the user's captures of both phases).

- **The output is 1920 pixels wide, not 1280.** `D1CRTC_H_TOTAL` read 0x897 and `D1CRTC_H_BLANK_START_END` 0x00BF083F,
  which libxenon writes as the total less one and as the sync offset over the active width plus that offset
  (xenos.c:344-346): 2200 pixels a line, 1920 of them shown. The kernel's scale factors fit 1920 by 1080: 0x00555560
  and 0x0071C720 in 0x65B4 and 0x65C4, a third and four ninths if 0x01000000 is 1.0, as libxenon writes it for 1:1,
  so 640 to 1920 and 480 to 1080. An earlier run's `VdQueryVideoMode` reported 1280x720 ("VdSwap writes only a
  fence"), so the console's output setting has changed since or differs with the user's capture card.
- **libxenon's settings show the surface 1:1.** With the scaler off, the viewport at 1280x720, 0x65E8 at 319 and the
  scale registers at 1.0, the whole image showed unscaled, its one-pixel border and all four corner squares in view,
  in the top left two thirds of the screen with black around it, as 1280x720 sits in 1920x1080.
- **The kernel's 640x480 scaling showed the top left 640x480 of the image,** twice as wide and half again as tall.
- **0x6024, libxenon's `AVIVO_D1CRTC_V_BLANK_START_END`, changed between reads,** and `D1CRTC_V_TOTAL` read 0, which
  libxenon also writes (xenos.c:340), so neither gives the output's height.
- **Putting the registers back returned the display to the kernel's mode,** and the console went back to Aurora.

## Scaling 1280x720 by hand scrambles it (2026-10-10)

`tests/probe_720p.c` showed the 1280x720 test image scaled to the console's output with the scaler on, the kernel's
tap control 0x505, scale factors of source over output in 8.24, and start phases from a formula that fits the
kernel's and libxenon's values, then with both phases 0 (log: `results/2026-10-10-probe-720p-2/xsdk.log`, with one
capture whose phase the user didn't say).

- **The output is 1920x1080 progressive by the kernel's own account:** `VdQueryVideoMode` gave 1920x1080, not
  interlaced, widescreen and hi-def, and `VdGetCurrentDisplayInformation` a 640x480 front buffer on a 1920x1080
  display, its scaler parameters a 640x480 source rectangle, a 1920x1080 output and filter type 1 both ways.
- **The scaler registers drop low bits.** The scale written as 0x00AAAAAA read back 0x00AAAAA0, and the phase written
  as 0x3D555 read 0x3D400, which fits the kernel's own values ending in zeros.
- **The image came out scrambled.** The capture shows a narrow copy of the image's frame, its border and all four
  corner squares striped every other line, across the left sixth of the screen, and the middle blocks larger in the
  middle. The same surface showed whole at 1:1 the run before, so the scaler, not the surface, is set wrong; which of
  its registers isn't known. The kernel builds the scaler's packets itself in `VdInitializeScalerCommandBuffer`
  (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:270-293), which the next probe asks it for.
- **Putting the registers back returned the kernel's scaling,** and the console went back to Aurora.

## The kernel's scaler packets name the registers but carry zeros (2026-10-10)

`tests/probe_720p.c` asked `VdInitializeScalerCommandBuffer` for 1280x720 to the reported 1920x1080 with the
kernel's own filter settings, logged the packets, applied their register writes through MMIO, showed the test image
for 12 seconds and wrote back what it had read from each register (log: `results/2026-10-10-probe-720p-3/xsdk.log`).

- **The call returned 43 dwords: type-0 headers over the registers libxenon sets, plus more, every value 0.** In
  order: 0x6590-0x6594 (scaler enable and taps), 0x6528, 0x6580-0x6584 (viewport start and size), 0x6120-0x6138
  (`D1GRPH` pitch to Y end), 0x6044-0x6050, 0x65E8, 0x65A0, 0x65B0-0x65C8 (taking in 0x65BC, which nothing here had
  written), 0x65DC and 0x6578. Six dwords followed that don't parse as packets: 0xFFFF995F, 0x195E, 0x00020000,
  0xFFFF995F, 0x1964, 0. Whether the values come later from elsewhere, or the arguments as Xenia guesses them are
  wrong, isn't known.
- **Applying the zeros blanked the screen,** viewport and surface sizes included, and it stayed black from launch to
  exit. That was the probe's fault: it applied the writes without checking them.
- **The console didn't come back to Aurora.** Besides the scaler, the probe had written back values it read from
  0x6044-0x6050 and 0x6578, which libxenon writes as 0 and may not read back what was written; that, or the zeros
  themselves, most likely left the display unable to recover.

So the scaler set the kernel builds includes 0x65BC and 0x6578, which the hand-set scaling left alone.

## 0x65BC isn't it, and calling the kernel's scaler builder breaks the way back (2026-10-10)

`tests/probe_720p.c` logged the kernel's starting values for its scaler set, asked `VdInitializeScalerCommandBuffer`
for its packets only to log them, then scaled the 1280x720 image as before and again with 0x65BC set, writing only
scaler registers (log: `results/2026-10-10-probe-720p-4/xsdk.log`, with one capture whose phase the user didn't say).

- **The kernel's own 640x480-to-1920x1080 setup:** 0x6590 1, 0x6594 0x505, 0x6528 0, viewport start 0 and size
  640x480, `D1GRPH` pitch and X end 640 and Y end 480, 0x65E8 0x9F, 0x65A0, 0x65B0, 0x65BC and 0x65DC 0, 0x65B4
  0x00555560, 0x65B8 0x3AA00, 0x65C0 0x01000000, 0x65C4 0x0071C720 and 0x65C8 0x3B800.
- **The packets carried zeros again,** the same 43 dwords, and the buffer passed as the tenth argument stayed zero.
- **0x65BC started at 0,** matching none of the scale or phase values, so the probe gave it the horizontal scale as a
  guess. The capture looks as scrambled as before; which phase it shows isn't known.
- **The console crashed after the title exited,** the screen black, and xbWatson reported it restarting. The probe
  wrote only registers the hand-set scaling had written, which went back to Aurora, plus 0x65BC, and put them back.
  The two runs that called `VdInitializeScalerCommandBuffer` both failed to return to Aurora, and the one that didn't
  call it returned, so the call itself may change the kernel's display state, perhaps from arguments it reads
  differently from Xenia's guess; that isn't shown. The SDK won't call it.

## What AMD's code says about the scaler (2026-10-10)

Not a hardware run: what public AMD sources say about this display block, to read the probes against. AMD's register
guides for the M56 and RV630 (x.org/docs/AMD/old) give the CRTC, graphics and LUT registers at the same offsets as
here (`D1CRTC_H_TOTAL` 0x6000, `D1GRPH_*` 0x6100-0x614C, `D1MODE_MASTER_UPDATE_LOCK` 0x60E0) but leave out
0x6500-0x65FF, the viewport and the scaler.

- **radeonhd scales on R5xx without scale factors or phases** (xf86-video-radeonhd:src/rhd_crtc.c:767-786,
  `DxScaleSet`): 0x65A4 0x00010001, tap control 0x101, and 0x00030100 in both 0x65B0 and 0x65C0. Its names for those
  registers are marked as guesses (rhd_regs.h:205-213). Its scaler update lock is bit 16 of 0x65CC (rhd_regs.h:412-413).
- **AMD's later display code names the fields** (linux:drivers/gpu/drm/amd/include/asic_reg/dce/dce_8_0_sh_mask.h,
  `SCL_*`). Tap control holds the taps less one, vertical in bits 0-2 and horizontal in 8-11. A filter control's bit 8
  selects a built-in 2-tap filter, and bit 0 nearest. The automatic mode control's bits 0 and 16 make the hardware work
  out the scale factors. The coefficient select takes the tap pair in bits 0-3, the phase in 8-11 and the filter type
  in 16-18. The update lock is bit 16, and bit 0 is the pending flag, which libxenon and these probes write. Read that
  way, radeonhd asks for 2 taps each way with the built-in filter and automatic scale factors, and the kernel's 0x505
  is 6 taps each way.
- **The same code starts each filter at (scale + taps + 1) / 2** (linux:drivers/gpu/drm/amd/display/dc/dce/
  dce_transform.c, `calculate_inits`). That is the formula that fit the kernel's and libxenon's phases, with the taps
  one more than the field. It loads coefficients by filter type, phase and tap pair through `SCL_COEF_RAM_SELECT` and
  `SCL_COEF_RAM_TAP_DATA` (`program_multi_taps_filter`). It also refuses vertical taps that aren't fewer than the lines
  the line buffer holds, which is the buffer's size over the source width (`dce_transform_get_optimal_number_of_taps`).
  A 1280-wide source fits half as many lines as the kernel's 640-wide one.
- **The kernel's six trailing dwords read as coefficient loads.** xenon-emu names 0x6578 `D1SCL_COEF_RAM_SELECT` and
  0x657C `D1SCL_COEF_RAM_TAP_DATA` (XenosRegisters.h:919-920). The tail selects filter type 0, then type 2
  (0x00020000). After each selection comes a header for 0x657C with bit 15 set, which writes every dword to the one
  register (xenia:src/xenia/gpu/command_processor.cc:651-656), and with its count bits all set. xenon-emu names 0x65BC
  `D1SCL_HORZ_FILTER_INIT_CHROMA` (XenosRegisters.h:929), which fits it not mattering for RGB.
- **The scrambled capture fits source lines landing in the wrong place.** Read again (`results/2026-10-10-probe-720p-2/
  capture.png`), each output line in the corner squares' rows shows either the red square at the left edge, about the
  right size, or the green one about a sixth of the way across, never both. So the horizontal source position changes
  from line to line, which a filter or a scale factor alone wouldn't do. Too many vertical taps for the line buffer
  would, as lines overwrite each other.

## The built-in 2-tap filter scales 1280x720 to 1920x1080 whole (2026-10-10)

`tests/probe_720p.c` scaled the 1280x720 test image with the scaler on at 1:1, then to the output with fewer and fewer
vertical taps, with 2 horizontal taps, and with the built-in 2-tap filter each way, each variant on its own
background tint, writing only scaler registers (log: `results/2026-10-10-probe-720p-5/xsdk.log`, with the xbWatson
lines from the launch, a cropped capture of the purple variant and a full-frame screenshot of it at 1280x720).

- **Only the built-in 2-tap filter came out whole,** by the user's eye: tap control 0x101, bit 8 set in both filter
  controls (0x65B0 0x00000100, 0x65C0 0x01000100), scale 0x00AAAAA0 both ways, phases 0x1D400. In the full-frame
  screenshot the white border shows on all four edges, the corner squares sit in all four corners, and the middle
  blocks span x 448-832 and y 296-423, where the source has them.
- **Every variant on the kernel's coefficient table came out scrambled:** 6 taps each way at 1:1 and scaled, 4, 3 and 2
  vertical taps with 6 horizontal, and 2 horizontal with 6 vertical. So the line buffer limit on vertical taps
  (above) doesn't explain it alone: 2 vertical taps with 6 horizontal still scrambled. 2 taps each way on the table
  wasn't tried, so whether the table or the tap count is at fault isn't known.
- **The log can't tell scrambled from whole.** Every write read back as written, less the low bits the scaler drops,
  and the registers read after each variant didn't change: 0x6524 0, 0x65CC 0, 0x65D0 1, 0x65E0 0.
- **At start:** 0x6520 0, 0x6524 0, 0x6548 0, 0x6550 0xFF (libxenon writes 0xFF there, xenos.c:392), 0x65A0 and
  0x65A4 0, 0x65B0 0, 0x65BC 0, 0x65C0 0x01000000, 0x65DC to 0x65E4 0, 0x65E8 0x9F, 0x6CBC 3 and 0x7118 0x780, which is
  1920 and which xenon-emu names `DC_LB_MEM_SIZE` (XenosRegisters.h:952).
- **Putting the registers back returned the kernel's scaling,** and the console went back to Aurora.
- **Neither of the display's own checks can see the scaled image.** The M56 guide places `DCP_CRC_*` (0x6C80-0x6C94)
  on the data going into the line buffer, before the scaler, and calls `D1CRTC_PIXEL_DATA_READBACK` (0x6078) a debug
  register for one-shot clocking. A test of the scaled output needs a capture.

## 720p in the test title: one late flip at the first vblank (2026-10-10)

`build/xsdk_test.xex` ran with every GPU test at 1280x720 on the display's scaler, and the new scale test (log:
`results/2026-10-10-720p/xsdk.log`, with the xbWatson lines from the launch). 108 checks passed and 3 failed.

- **The 1280x720 front buffers work.** The present, triangle, texture and render tests passed every check, with 0
  wrong pixels in the texture and render tests and 258720 pixels drawn for a triangle of 259200.
- **The scaler took the SDK's settings and gave the kernel's back.** With the update lock at bit 16, the scale test read
  back tap control 0x101, the built-in filter bit in both filter controls, and the output's scale factors and phases
  100 ms after showing, then the kernel's values 100 ms after closing. 0x652C, which libxenon names
  `AVIVO_D1MODE_DESKTOP_HEIGHT`, read 1080.
- **The 3 failures were one late flip each, in the three pacing modes whose first flip came at vblank 1.** The mode
  whose first flip came at vblank 2 had none, and every mode missed no vblank and flipped every swap interval. At
  640x480 a first flip at vblank 1 wasn't late (`results/2026-10-10-milestone-9/xsdk.log`).
- **Why isn't shown.** The late check compares 0x6530, the line counter, with the front buffer's height. The counter
  runs in viewport lines: under the kernel's 480-line viewport its two calls a frame came 28 lines and 930 us apart,
  63 lines of the 1080p output ("The vblank callback, flips and giving the callback back"), so it stays below about
  500 there (1125 output lines times 480/1080). The SDK switches the scaler to 720 lines just before pacing starts. If
  the switch hadn't taken by the first vblank, the counter would still read on the old scale, under 720, and the flip
  would count as late whether or not it tore. So the first 1280x720 show now waits until the counter passes 720 and
  logs how long that took, and a late flip records its vblank and line.

## 720p at 1080p output passes clean, after waiting for the scaler (2026-10-10)

`build/xsdk_test.xex` ran again with the first 1280x720 show waiting until the line counter passed 720, and late flips
recording where they came (log: `results/2026-10-10-720p-2/xsdk.log`, with the user's screenshot of the scale test at
1280x720). All 111 checks passed, and the console went back to Aurora.

- **The counter took 18154 to 32559 us to reach 720 after each switch:** nine opens read 27794, 30122, 18154, 30123,
  22392, 32559, 30048, 29562 and 24900 us. That fits the new size taking at the next vblank, up to a frame (16.7 ms)
  away, and the counter then reaching 720 one active display later, 720 of 750 lines or about 16.0 ms: 16.0 to 32.7
  ms in all. So the first vblank after a switch still counts on the old scale, which is what the late flips needed.
  The next run's shortest wait, 16492 us, also falls in that range.
- **No flip was late.** Three of the four pacing modes made their first flip at vblank 1, where each had one late flip
  in the run before; all four had 0 missed and 0 late.
- **The scale test's screenshot shows the image whole and in place:** the blue-grey background, the white border on all
  four edges, the corner squares in all four corners, and the middle blocks at x 448-832 and y 296-423, as in the
  source.
- **Not covered by this run:** a title asking for `XSDK_DISPLAY_KERNEL`, which no test opened, and 1:1 at a 720p output,
  as the console outputs 1080p.

## The kernel's mode through the SDK, at 1080p output (2026-10-10)

`build/xsdk_test.xex` ran with the scale test also opening `XSDK_DISPLAY_KERNEL` (log:
`results/2026-10-10-720p-3/xsdk.log`). All 115 checks passed.

- **The kernel's mode opened at the kernel's 640x480 and left the scaler alone,** both 100 ms after showing a buffer
  and 100 ms after closing.
- **The 720p checks passed again,** with no late flip, and the counter reached 720 between 16492 and 32553 us after
  each switch.

## 720p at 720p output, and 720p done, item 10 (2026-10-10)

`build/xsdk_test.xex` ran with the console's video output set to 720p (log: `results/2026-10-10-720p-4/xsdk.log`,
with the user's screenshot of the scale test). All 115 checks passed, and the console went back to Aurora.

- **The kernel reported a 1280x720 output, and the SDK showed 1280x720 1:1 with the scaler off,** as libxenon does.
  The scale test read back the settings it expected for that, and `AVIVO_D1MODE_DESKTOP_HEIGHT` read 720.
- **The screenshot shows the whole 720p frame:** the blue-grey image, its border on all four edges, the corner squares
  and the middle blocks. The user's OBS canvas is 1920x1080, so a 720p signal sits 1:1 in its top-left, and the
  1280x720 screenshot shows the frame as its top-left 853x480.
- **Pacing at 720p output had no missed or late flip,** and the counter reached 720 19069 to 31152 us after each
  switch, inside the 16.0 to 32.7 ms worked out above.
- **The kernel's mode again opened at the kernel's size and left the scaler alone.**

With the 1080p runs above, item 10 is done. Titles get a 1280x720 front buffer by default: scaled to a 1080p output by
the display's built-in 2-tap filter, shown 1:1 on a 720p output, and the kernel's 640x480 when they ask for it. The
GPU tests pass at 1280x720, the captures show the image whole, and the console goes back to Aurora. Interlaced outputs
aren't tried and fall back to the kernel's mode.

## Ten minutes of paced frames at 720p (2026-10-10)

`build/pacing_test.xex` ran its 10-minute mode (3 buffers, a flip every vblank) with the 1280x720 front buffer scaled
to the 1080p output (log: `results/2026-10-10-pacing-720p/xsdk.log`). All 8 checks passed.

- **36093 frames, 36093 flips over 36094 vblanks: 0 missed, 0 late, 0 frames wrong.** The longest wait for a front
  buffer was 17950 us, as at 640x480 ("Ten minutes of paced frames, milestone 9").
- **The line counter reached 720 30702 us after the switch,** inside the 16.0 to 32.7 ms worked out above.
- **Every frame checked held its bar.** Frame 0 is still left out of the check, as since the 640x480 fix, because its
  background is whatever eDRAM held.

So milestone 9's pacing holds at the 720p default.

## Shader constants: partial writes hang the GPU, libxenon's whole blocks don't (2026-10-10)

`build/xsdk_test.xex` ran milestone 11's `test_constants` twice. Each time it set three float constants per stage,
one boolean word and one loop constant per draw with the first `xsdk_gpu_set_*_constants`, and each time a fence never
landed and the GPU was kept. The user sent the end of each log (`results/2026-10-10-milestone-11/`). The second
showed the SDK's constant split read back as set and no checked frame wrong, but not which frame hung.

`tests/probe_constants.c` then stepped through the features one draw at a time, each with its own fence (log:
`results/2026-10-10-probe-constants/xsdk.log`, with the whole paste in `paste.txt`).

- **XAM leaves the constant split the SDK sets:** `SQ_VS_CONST` 0x000FF000 and `SQ_PS_CONST` 0x000FF100, vertex
  shaders' 256 constants from 0 and pixel shaders' from 256, as libxenon's setters assume (xe.c:2013-2029).
- **Every shader feature works on constants written libxenon's way.** That means aligned whole blocks: 16 float
  constants at a time, and all 40 boolean and loop dwords at once (xe.c:1217-1221, 1320-1369). The vertex shader read
  c2 and placed a quad with `mad oPos, r1, c0, c1`, two constants in one instruction. The pixel shader read c0.
  `cexec b128` skipped its block with the boolean clear and ran it with it set. `loop i0` ran 0, 1 and 3 passes as
  loop constant 0 said. Each centre held the expected colour. test_constants's six quads, every kind of constant set
  per draw, drew right for 30 frames.
- **Writing only what changed hung on the first frame,** still aligned. That was three float constants per stage, a
  lone boolean word and a lone loop dword. After the hang the command processor had read everything (read pointer =
  write pointer = 0x1016) and `RBBM_STATUS` read busy, so the hang is in the pipeline, not in fetching the packets.
- **Which of the partial writes hangs isn't shown,** the float constants or the lone boolean or loop word, nor whether
  whole blocks need the alignment. The SDK now writes exactly as the passing step did: it keeps a copy of every
  constant, rewrites each 16-constant block a change touches, and rewrites all 40 boolean and loop dwords for any
  change, each packet aligned as libxenon aligns it.
- **The boolean and loop results are the first hardware proof** of those xost encodings: its feature notes list
  boolean constants as unproven (xenos-toolchain/docs/feature-tests.md).

## Shader constants, milestone 11 (2026-10-10)

`build/xsdk_test.xex` ran with the SDK writing constants in whole aligned blocks (log:
`results/2026-10-10-milestone-11-2/xsdk.log`, with the xbWatson lines from the launch and the user's capture). All 124
checks passed, and the console went back to Aurora.

- **test_constants passed:** 6 quads in each of 60 frames, each placed and coloured only by its constants. Every
  checked point was right in all 59 checked frames: the centre and 4 points inside each quad, and 2 outside.
- **Each quad's centre in the capture matches its colour** within a few units of the capture's conversion: BA0201 for
  C00000, 02BE01 for 00C000, 42FD03 for 40FF00 (the boolean), 3F3F3F for 404040 (one loop pass), 0001B9 for 0000C0
  (three), FA817F for FF8080 (all of them). The background reads 202020.
- **The vertex/pixel constant split read XAM's 0x000FF000 and 0x000FF100** before the test and the same after.

With that, milestone 11 is done. Titles set float, boolean and loop constants per draw. Each kind drives a quad on
the console, read back by pixel and seen on a capture, and the test fails when any kind is written wrong.

## Indexed draws and topologies, milestone 12 (2026-10-10)

`build/xsdk_test.xex` ran with `test_indexed`, which draws one rectangle eight ways from index buffers (log:
`results/2026-10-10-milestone-12/xsdk.log`, with the user's screenshot of the last frame at 1280x720). All 133 checks
passed, and the console went back to Aurora.

- **Every way drew its shape in its cell in all 59 checked frames:**
  - triangle lists and strips on 16- and 32-bit indices, filled to their edges and not past them;
  - the outline as a line list on 16-bit indices and as a line strip on 32-bit, each edge found within 2 pixels and
    the inside clear;
  - the corners and centre as a point list, each point within 2 pixels and nothing between them;
  - a triangle list on 32-bit indices past 65535.
- **The vertices were stored out of order,** so each shape needed its indices followed. The past-65535 list's
  16-bit truncation would have landed on degenerate vertices and drawn nothing, so the console reads all 32 bits.
- **The screenshot shows each shape:** the filled rectangles, single-pixel outlines with clear insides, and white
  single-pixel points at the corners and centre of the seventh cell. Its colours are further from the targets than
  earlier captures (CC3141 for E04040, EC7A06 for FF8000), which the log's exact pixel checks rule out as a drawing
  fault.
- **Indices go to the GPU as libxenon sends them** (xe.c:191-200). The size is in 16-bit words with an 8-in-16 swap,
  which 32-bit indices shift to 8-in-32. The vertex cache is invalidated over the indices first.

With that, milestone 12 is done.

## Depth and stencil, milestone 13 (2026-10-10)

`build/xsdk_test.xex` ran with `test_depth` (log: `results/2026-10-10-milestone-13/xsdk.log`, with the user's
screenshot of the last depth-tested frame). All 143 checks passed, and the console went back to Aurora.

- **With the depth test on, all 59 checked frames were right.** Each resolve cleared depth to 0.5 and stencil to 0.
  - **Back to front:** the near rectangle covered the far one.
  - **Front to back:** the far rectangle stayed out of the near one's area and showed only beyond it.
  - **Stencil:** a rectangle tested for stencil equal to 1 drew only where a mask had replaced the stencil with 1.
  - **The cleared depth:** a rectangle at 0.75 stayed hidden behind it, and one at 0.25 showed.
- **With the depth test off, all 9 checked frames got exactly the two depth checks wrong:** the far rectangle in the
  front-to-back overlap, and the one behind the cleared depth. Every other check stayed right.
- **The screenshot shows the same at each check point,** with the colour shift of the last capture (3D33D0 for 4040E0
  in the front-to-back overlap). The far rectangle is cut where the near one is, magenta fills only the mask, and the
  rectangle behind the cleared depth doesn't show.
- **Setting the depth and stencil on their own works.** The SDK writes `RB_DEPTHCONTROL` and `RB_STENCILREFMASK` per
  draw by themselves, where libxenon writes `RB_DEPTHCONTROL` within its 9-register control block (xe.c:1372-1376),
  and the GPU took them.

With that, milestone 13 is done.

## Raster and blend state, milestone 14 (2026-10-10)

`build/xsdk_test.xex` ran with `test_raster` (log: `results/2026-10-10-milestone-14/xsdk.log`, with the user's
screenshot of the last frame). All 152 checks passed, and the console went back to Aurora.

- **All 59 checked frames held all 24 points.** Each point was worked out on the CPU, the blends within 2 per channel.
  - **Blends over a base:** alpha (half-transparent orange over blue, 80807F), additive with green clamped (C0FFD0),
    source less destination with green going negative and clamping to 0 (A00060), and max (C0D090).
  - **The write mask:** only the red and blue of a magenta overlay landed on green, giving E0E0E0.
  - **Culling, with front faces clockwise:** culling back drew the clockwise rectangle and dropped the mirrored one,
    and culling front did the reverse.
  - **The scissor:** its top-left and bottom-right pixels drew, and the pixel just left of it and the one just below
    it didn't. The bottom-right corner is exclusive, at least in y; no point checks just right of it.
  - **The viewport:** a draw over all of clip space filled the viewport's rectangle and nothing beside or below it.
  - **Depth bias:** over a base at depth 0.5 under a less-than test, an unbiased rectangle at the same depth stayed
    hidden, and the same rectangle biased by -0.01 showed.
- **The depth clear of 1.0 holds.** The base in the depth bias cell drew, which a depth clear of 0 would have stopped;
  that was the float rounding fixed before the run.
- **The screenshot shows the same at each check point.** The greys and the lighter blends land within a few steps of
  their targets (80807D, C2FFCF, BFD592, E0E0E0, 7E7E7E). Saturated colours shift as in earlier captures (CB3040 for
  E04040, 8D0059 for A00060, EC7A06 for FF8000). The screenshot's edges are only good to a few pixels, after the
  scaler and the crop. The log's checks pin the scissor's edges to the pixel, and the viewport's to within 3 pixels.
- **Setting blend and raster state on their own works.** The SDK writes `RB_BLENDCONTROL0`, `RB_COLOR_MASK`,
  `PA_SU_SC_MODE_CNTL` and the four `PA_SU_POLY_OFFSET` registers per draw, and the scissor and viewport between
  draws. libxenon packs the blend and cull fields into its control block (xe.c:1451-1455, 1505-1511). The GPU took the
  SDK's separate writes mid-frame.
- **The slope term of depth bias isn't tested.** The API takes it, but this run only used the constant term.

With that, milestone 14 is done.

## A small texture's mips share its base's tail (2026-10-10)

Milestone 15's first run of `build/xsdk_test.xex` passed 163 of 164 checks (log tail:
`results/2026-10-10-milestone-15/xsdk-run-1-tail.log`). Only the 16x16 tiled texture failed, and only its levels 1 to
4. Its base, read from inside the packed tail, was right. Every pixel of the four levels held an earlier test's clear
colour, read as a texel. GPU memory isn't cleared when allocated, so the GPU had read texels the test never wrote.

`build/probe_mip_tail.xex` settled where those levels are (log: `results/2026-10-10-probe-mip-tail/xsdk.log`).

- **The setup:** six tiled textures whose short side is 16 or less (16x16, 8x8, 4x4, 16x8, 8x16 and 32x16), drawn one
  texel to a pixel at every level. Every texel of four pages around each texture held a code for its page and place.
- **With the mip address on the page after the base, every level came from the base's page.** None came from the mip
  page or from outside the four pages.
- **With the mip address set to the base, every level came from the same texels as before.** So the console ignores the
  mip address once the base is in the tail.
- **Every level read one rectangle at Xenia's tail offsets** (xenia:src/xenia/gpu/texture_util.cc:116-207). That
  includes the base at (16, 0) for a square and (0, 16) for a wide texture. It matches Xenia's own run of the probe,
  page aside.
- **Xenia is wrong to give these mips a tail of their own at the mip address**
  (xenia:src/xenia/gpu/texture_util.h:85-98). Xenia reads from the mip address, so the probe's first setup would pass
  in Xenia and fail on the console.

The SDK now keeps such a texture's mips in its base's tail, and points the mip address at the base so Xenia reads them
there too. `test_sampling` adds a 16x8 texture to the 16x16 one, and 32x32 and 24x20 textures, whose tail starts at
level 1, the case Xenia's note sets apart next (texture_util.h:91-94).

## Sampling and mipmaps, milestone 15 (2026-10-10)

The second run of `build/xsdk_test.xex` ran with `test_sampling` and the small-texture fix (log:
`results/2026-10-10-milestone-15-2/xsdk.log`, with the user's screenshot of the last frame). All 164 checks passed,
and the console went back to Aurora.

- **All 59 checked frames held all 560 points, and every pixel of the last frame was right:** 221,775 pixels across 56
  cells and the ring of clear pixels around each.
- **Mip levels, drawn one texel to a pixel, came from where the SDK put them:**
  - **128x128:** the packed tail from level 3.
  - **136x68 and 68x136:** each level's rows and pitch sized as if the base were 256x128 or 128x256, and the tail laid
    out wide or tall.
  - **32x32 and 24x20:** the tail from level 1.
  - **16x16 and 16x8:** the base and its mips in one tail.
  - **16x16 linear, with one level:** its base in the tail at (16, 0) with a 64-texel pitch.
- **The GPU picked each level from the drawn size.** The non-square textures' levels came to up to 0.32 above a whole
  level, and still read that level. Nothing here tells rounding to the nearer level from rounding down.
- **Filters, each with the other filter of the pair set the other way:**
  - **Magnified four times:** point matched exactly; linear stayed within 3 per channel of a bilinear blend around texel
    centres.
  - **Minified twice, a quarter texel in:** point matched exactly; linear stayed within 3 of weights of 1/4 and 3/4.
  - **So the GPU took the magnification filter below level 0 and the minification filter above it.**
- **Mip filters, at level 1 biased by the fetch constant's 12/32:** point read level 1 alone, and linear blended 3/8 of
  level 2 into it, within 3.
- **Addressing, with half the texture past each edge:** wrap, mirror and clamp were right, and so was mirroring across
  with clamping down. So the u and v modes sit where the SDK writes them.
- **The screenshot shows each level's hue** where a level is big enough to sample. The 1- and 2-pixel levels blur in
  the screenshot after the scaler; the log checks them.
- **Not covered:**
  - **Anisotropic filtering:** it waits, as SCOPE.md says.
  - **The border and mirror-once modes:** not offered.
  - **Linear textures with mips:** creating one fails with `STATUS_NOT_IMPLEMENTED`.
  - **A level of detail exactly halfway between two levels:** avoided.
  - **The precision of linear filtering:** only bounded by the tolerance of 3.

With that, milestone 15 is done.

## Texture formats, milestone 16 (2026-10-11)

`build/xsdk_test.xex` ran with `test_formats` (log: `results/2026-10-11-milestone-16/xsdk.log`, with the user's
screenshot of the last frame). All 175 checks passed, and the console went back to Aurora.

- **All 59 checked frames held all 620 points, and every pixel of the last frame was within tolerance:** 123,102 pixels
  across 62 cells and the ring of clear pixels around each.
- **Every format read back right, linear and tiled:** A8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, A2B10G10R10, R8, R8G8,
  R16F, R16G16F, R16G16B16A16F, DXT1, DXT3 and DXT5.
- **The fixed-point and half-float formats matched the CPU's decode exactly,** alpha included: the largest error in any
  channel was 0.
  - The 4-, 5-, 6- and 10-bit channels and the 1- and 2-bit alphas came out as the value times 255 over its maximum,
    rounded.
  - Halves of 2.0 and -1.0 clamped to 255 and 0 in the 8-bit target.
- **So each format's endian swap and component order are right.**
  - **Endian swaps:** 8-in-16 for 16-bit units, half floats and DXT's words; 8-in-32 for 32-bit units; none for bytes.
  - **Component order:** x is the lowest bits or the first byte, as Xenia maps the formats to Vulkan.
- **DXT decoded as the CPU worked it out:**
  - **Endpoints and DXT1's transparent black came out exact,** within the tolerance of 1.
  - **Blends came within 3 (DXT1, DXT3) and 4 (DXT5)** of exact thirds, halves, sevenths and fifths. Xenia's host
    GPU was off by up to 6.
  - DXT1's three-colour mode with transparent black works, and so do DXT5's six-alpha mode with 0 and 255, and DXT3's
    4-bit alphas.
- **Mips work in blocks:** every level of 32x32 tiled R8, R5G6B5, A2B10G10R10, R16G16B16A16F, DXT1 and DXT5 textures
  read right. That covers 1-, 2-, 4- and 8-byte texels, and 8- and 16-byte blocks, each with its tail from level 1 and
  DXT's tail offsets in blocks.
- **The screenshot's linear and tiled rows match each other,** within the screenshot's own scaling noise: at most 10 in
  a channel, 2.2 on average.
- **Not covered:**
  - DXT3 and DXT5 colour blocks with the first endpoint not above the second: the test kept them four-colour.
  - Signed and gamma textures.
  - The formats outside the milestone, such as DXN, 16-bit fixed point and 32-bit float.
  - Linear filtering of these formats: the test point-sampled.
  - Whether linear rows need 256-byte alignment: the SDK always uses it, so nothing tested less.

With that, milestone 16 is done.

## Vertex input, milestone 17 (2026-10-11)

`build/xsdk_test.xex` ran with `test_vertex` (log: `results/2026-10-11-milestone-17/xsdk.log`, with the user's
screenshot of the last frame). All 183 checks passed, and the console went back to Aurora.

- **All 59 checked frames held all 115 points, and every pixel of the last frame was right:** 100,188 pixels across 23
  cells and the ring of clear pixels around each.
- **Every vertex format read right as a colour,** each square within 1 of the CPU's decode, alpha included:
  - float1 to float4;
  - half2 and half4;
  - byte4, short2 and short4, unsigned and signed, normalised and whole numbers.
- **Sign and scale came out right.**
  - Each signed format's negative clamped to 0 in the 8-bit target.
  - Each whole-number format's 2 clamped to 255, where a normalised read would have given nearly 0.
  - So the fetch's sign and number-format bits sit where the SDK writes them. This is the first hardware result for
    byte vertex formats, which xost lists as unproven (xost:README.md:89-91).
- **Components a format lacks read 0, and w read 1, as libxenon patches them** (xe.c:900-916).
  - float1, float2, float3, half2 and the short2 formats drew alpha 255 as colours.
  - Positions in half2, short2n, float2 and float3 drew in place, which needs w to be 1.
- **Streams 0 to 6 and 15 worked, each fetch reading the constant 95 minus its stream.** They span fetch constant
  groups 31, 30, 29 and 26.
  - Positions came from streams 0, 3 or 15 and colours from 1, 2 or 4, with strides from 4 to 28 bytes and offsets of
    0 or 4.
  - Writing a stream sent its whole group from a kept copy, and the other streams in that group stayed intact.
  - One cell read position and colour from a single stream, 24 bytes a vertex.
- **The screenshot shows each cell's four squares,** the formats without blue and alpha in reds and greens.

With that, milestone 17 is done.

## Recorded command buffers, milestone 18 (2026-10-11)

`build/xsdk_test.xex` ran with `test_replay` (log: `results/2026-10-11-milestone-18/xsdk.log`). All 193 checks
passed, and the console went back to Aurora.

- **The GPU ran packets from memory with `PM4_INDIRECT_BUFFER`,** two dwords after the header: the buffer's GPU
  address, then its length in dwords, as Xenia reads them (xenia:src/xenia/gpu/command_processor.cc:943-953).
- **The recordings:**
  - **A background of 86 dwords:** the target, the shaders, stream 0 and a draw of four squares, recorded once.
  - **A bar of 6 dwords:** one draw, recorded once.
- **Each of 120 frames ran the background once and the bar twice.** Before each bar, the ring pointed stream 0 at a
  different moving bar.
  - So one buffer ran more than once in a frame, and several ran in a frame.
  - State set on the ring between runs carried into the recorded draws.
- **Every frame came back with both bars where its frame put them, and all four squares.** None of the reused buffers
  held the wrong frame.
- **Milestone 9's checks held through the present path,** with 2 buffers flipping every vblank. The 120 frames took
  120 flips over 120 vblanks, with none missed and none late, and the longest wait was 17,948 us.
- **The recordings' padded constant writes still aligned,** as the buffers are page-aligned like the ring.
- **A recording that outgrew its 4-dword buffer** failed with `STATUS_BUFFER_TOO_SMALL` and left the buffer empty.
- **Not covered:**
  - running a buffer from inside another;
  - `PM4_INDIRECT_BUFFER_PFD`;
  - buffers large enough to take long to run.

With that, milestone 18 is done, and so are all of milestones 11 to 18.

## The splash stays up until present's first frame (2026-10-11)

Two runs, at `7420d94` (logs: `results/2026-10-11-splash-handover/`):
- **vulkan4xenon's milestone 1 title** (`vulkan4xenon_test.log`). It shows the splash with `xsdk_splash_begin` from
  `vkCreateDevice`, and its swapchain is `xsdk_present`. All 20 checks passed, and the console went back to Aurora.
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`, the tail the user sent). All 194 checks passed, the splash's
  among them: `xsdk_splash_show` is now begin, a hold and end, and this was its first run on the console.

What the first run showed:
- **`xsdk_splash_begin` put the logo on screen.** `D1GRPH_PRIMARY_SURFACE_ADDRESS` went from XAM's 640x480 surface at
  0 (see "At title start the display shows a 640x480 surface at address 0") to the splash's buffer at 0x1F4F0000.
- **`xsdk_present_open` took the display over without showing its own black buffer.** Afterwards the display still
  showed 0x1F4F0000, which isn't one of present's buffers.
- **The first present flipped to buffer 0** (0x1F150000), whose centre read the app's clear colour exactly (00 60
  00).
- **The present chain kept going afterwards.** The splash's buffers are freed a vblank after the first flip; that
  step isn't logged. All 120 later frames were shown: 121 flips over 132 vblanks, none late. The 10 vblanks missed
  came from the title, which waits for each frame and reads it back before presenting it. The logo's frame was freed
  only after it had left the screen, and nothing hung.
- **`D1GRPH_PRIMARY_SURFACE_ADDRESS` reads back the buffer on screen** (`xsdk_display_shown_address`): each read
  matched the buffer the test expected.
- **On screen,** the user reported for the first run: the logo faded in, held for a second or two, the app's dark
  green flashed, then its triangle showed. The titles check which buffer is shown, not what's in it.
- **Not covered:** `xsdk_splash_end` with the splash still up, which a device closed before any swapchain would take.

## Textures in memory the caller owns, and byte-order RGBA (2026-10-11)

Two runs, at `7ff6c4c` (logs: `results/2026-10-11-texture-attach/`):
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`, the tail the user sent). All 194 checks passed.
  - `xsdk_texture_create`, now `xsdk_texture_init`, an allocation and `xsdk_texture_attach`, made every texture the
    earlier milestones' tests sample.
  - `test_formats` passed with its 66 cells, which include `XSDK_FORMAT_R8G8B8A8` and `XSDK_FORMAT_B8G8R8A8`, linear
    and tiled.
- **vulkan4xenon's milestone 2 title** (`vulkan4xenon_test.log`). All 33 checks passed. Its images are
  `xsdk_texture_init`, then `xsdk_texture_attach` at an offset into memory the app allocated. The two byte-order
  formats showed every texel of a 4x4 image exactly: red and blue in place, each format's bytes unswapped
  (k_8_8_8_8, `ENDIAN_NONE`).

## Triangle fans, and raster facts through vulkan4xenon (2026-10-11)

vulkan4xenon's milestone 3 title ran at xsdk `604bdfb` (log and two captures: `results/2026-10-11-fans-and-raster/`).
All 45 checks passed, and the console went back to Aurora. It drives xsdk's state blocks through Vulkan. Along the
way it settled what xsdk's own tests hadn't:

- **Triangle fans draw.** `XSDK_PRIMITIVE_TRIANGLE_FAN` (Xenos's `kTriangleFan`, xenia:src/xenia/gpu/xenos.h:50), on
  16-bit indices: four indices from the top left filled the quad to its edges.
- **Facing is decided after the viewport.** vulkan4xenon's viewport has a positive y scale, the reverse of xsdk's.
  Culling still followed winding as the triangle looks on screen, clockwise with `front_clockwise` set.
- **Pixel centres sit on whole coordinates under `PIX_CENTER_D3D`.** With the viewport moved half a pixel up and
  left, edges at x = 600.25 and 700.25 covered columns 600 to 699, and the same in y. That's what centres on halves
  give for Vulkan's coordinates, and what Xenia says of `kD3DZero` (xenia:src/xenia/gpu/xenos.h:861-866).
- **The scissor's right edge is exclusive too.** Milestone 14 showed the bottom edge only. Here, a 60 by 50 scissor at
  (280, 510) drew pixel (339, 559), but not (340, 535) or (310, 560).
- **Reverse subtract and min blend** as their names say, within 2 per channel: destination less source, and the
  smaller of each. Milestone 14 had covered alpha, additive, subtract and max.
- **A colour write mask of 0 with stencil replace** wrote the stencil and no colour.
- **Depth bias in depth units:** -0.01 pulled a quad at 0.3 in front of an equal-depth one under less-than, as in
  milestone 14. Here it was given as -0.01 x 2^24 in Vulkan's units and divided back.

## Placed targets, resolves that clear less, and byte-order resolves, through vulkan4xenon (2026-10-11)

Two runs, at `c5d87ed` (logs and a capture: `results/2026-10-11-render-to-texture/`):
- **`build/xsdk_test.xex`** (`xsdk_test.log`). All 194 checks passed, and the console went back to Aurora, so the
  existing callers lost nothing: `xsdk_gpu_set_target` and the resolves still write what they did.
- **vulkan4xenon's milestone 4 title** (`vulkan4xenon_test.log`, `render_to_texture.png`). All 58 checks passed, and
  the console went back to Aurora.

That title's eight render passes a frame, for 60 frames, ran on the xsdk additions made for them:

- **Targets placed with `xsdk_gpu_set_target_at`:** colour from tile 0 and depth from 2048 less the target's tiles,
  for a 200x120 target with depth, colour-only targets of 96x64 and 64x32, and the 1280x720 swapchain. Depth tested
  right at the top of eDRAM.
- **A resolve that clears neither leaves eDRAM as it was** (`xsdk_gpu_set_resolve_clears(false, false)`):
  - depth survived a colour-only target's draws and resolve between two passes: a quad at depth 0.45 stayed hidden
    behind one at 0.3 drawn two passes before;
  - colour survived for the next pass, which drew over it without drawing it back.
- **Byte-order resolves** (`xsdk_gpu_resolve_texture`): R8G8B8A8 without the red-blue swap and B8G8R8A8 with it. Read
  back from memory and sampled, red and blue were in place, within 2 per channel.
- **Resolving the pitch:** a 200-wide texture resolved across its 224-texel pitch, and its column 199 held what was
  drawn there.
- **Sampling straight after a resolve** into the same texture, in the same submission, as milestone 8 did, now for
  the byte-order formats. A front buffer also sampled as a tiled B8G8R8A8 texture of its size.
- **Xenia differs:** in vulkan4xenon's title, a draw sampling an image resolved into earlier in the run got its
  earlier contents, even across submissions, though `test_render` passes there with an R8G8B8A8 target too. The
  console is right; the cause in Xenia isn't known.

## Sleeps, write speeds, and a set-up that wouldn't run from a cached stream (2026-10-11)

vulkan4xenon's vkbench, at xsdk `8859829` (log: `results/2026-10-11-milestone-19-stream/vkbench.log`), for milestone
19. Its probes ran before the GPU opened:

- **A 1 ms sleep took 1993 us on average,** from 1839 to 1999 over 100 (`xsdk_thread_sleep(1)`, so
  `KeDelayExecutionThread`). So the ring's wait for room, which slept 1 ms at a time, cost about 2 ms a wait.
- **Writing 1 MB, fastest of 8:**
  - into write-combined memory with volatile 32-bit stores: 291 us, 3603 MB/s;
  - into write-combined memory with newlib's `memcpy`: 5658 us, 185 MB/s, 20 times slower;
  - into cached memory with 32-bit stores, 667 us, then `dcbst` over it, 94 us: 1377 MB/s in all.
- **The set-up never finished from a cached stream.** At that commit every packet, `xsdk_gpu_set_up`'s included,
  went into 4 MB of cached memory, written out with `dcbst` and run from the ring with `PM4_INDIRECT_BUFFER` after a
  vertex cache invalidate, as libxenon runs its own (libxenon:drivers/xenos/xe.c:52-75, 83-147). The set-up's fence
  didn't land within 1 s, so `xsdk_gpu_open` returned `STATUS_IO_TIMEOUT`. Whether the console refused the cached
  memory or the set-up's packets in an indirect buffer isn't isolated. libxenon loads its own microcode (xe.c:224-252,
  336-343); the SDK runs the kernel's.

So the set-up goes straight into the ring again, as before, and the stream moved to write-combined memory, where
milestone 18's recorded buffers ran and 32-bit stores were faster.

## A write-combined stream ran its first segment and hung on the next (2026-10-11)

vkbench again, at xsdk `f1d385c` (log: `results/2026-10-11-milestone-19-stream-2/vkbench.log`, the whole of it; the
title hung on the boot logo). The set-up went straight into the ring and finished, and later packets went into a
write-combined stream.

- **The probes repeated:** a 1 ms sleep took 1990 us on average; write-combined 32-bit stores again ran at 3603 MB/s,
  `memcpy` at 185 MB/s, and cached stores with `dcbst` at 1413 MB/s.
- **The stream's first segment ran:** the xsdk backend's start waits for a resolve and a fence written into the stream
  (`clear_first`), and it passed. That segment started at the stream's offset 0.
- **The first frame never finished.** Its segments started wherever the one before ended, at any dword. libxenon starts
  each of its segments on a 128-byte line (libxenon:drivers/xenos/xe.c:123), and milestone 18's buffers were
  page-aligned. So the stream now starts each segment on a 128-byte line; the next run tells whether that was all.

## Segments on 128-byte lines run, and the waiting is gone (2026-10-11)

vkbench at xsdk `b5933a0` (log and CSVs: `results/2026-10-11-milestone-19-stream-3/`). All 14 checks passed, both
backends drew the same picture in every scene, and the console went back to Aurora. The only change from the run that
hung was each stream segment starting on a 128-byte line, so that was the cause.

- **No waits:** in every scene, the ring and the stream never made the CPU wait. `draws_4000` ran 602.6 segments a
  frame, 156.4 dwords a cube.
- **Draw and submit, median, against the first run** (vulkan4xenon:results/2026-10-11-milestone-5/vkbench_run1.log):
  - xsdk `draws_1000` 1,213 us, from 37,590, so 1.21 us a cube; `draws_4000` 4,836 us, from 151,190; `states_1000`
    2,167 us, from 55,496;
  - vulkan4xenon `draws_1000` 2,867 us, from 47,515; `draws_4000` 11,703 us, from 195,871.
- **The GPU still finished 0.3 ms after the submit** in the cube scenes, and the GPU-bound scenes kept their times.
- **Reading uniform buffers is now the largest cost through vulkan4xenon:** `uniforms_1000` took 6,852 us against
  `draws_1000`'s 2,867, 4.0 us a draw for its two blocks read from mapped write-combined memory at submit. The first
  run measured 1.0 us.

## Every test passes on the stream (2026-10-11)

Two runs at xsdk `5a06bc3` (the tails the user sent: `results/2026-10-11-milestone-19-regression/`):
- **`build/xsdk_test.xex`:** all 197 checks passed, the 194 from before and the three for the counters and the
  stream, and the title exited with status 0. So every drawing test, milestone 18's recorded buffers among them, ran
  from the stream.
- **vulkan4xenon's test title:** all 58 checks passed, and the title exited with status 0.

With the run before, milestone 19's done-when holds on the console: `draws_1000` drew and submitted in 1,213 us,
under 16 ms; `draws_4000` is recorded with its counts; both titles pass in full; and vkbench's backends drew the same
pictures. Its third step, fewer dwords a draw, hasn't been started.

## Copies into write-combined and cached memory (2026-10-11)

vkbench at vulkan4xenon `86866e8`, xsdk `a465595` (log: `results/2026-10-11-milestone-20-probe/vkbench.log`), for
milestone 20. All 14 checks passed again, with draw and submit times within 4% of the run before. Its probes copied
1 MB from memory, flushing the source from the cache with `dcbf` before each of 8 rounds, and every copy came out
right:

- **Into write-combined memory:** newlib's `memcpy` 185 MB/s; 32-bit stores with `dcbt` 517 MB/s; 64-bit stores with
  `dcbt` 883 MB/s; VMX stores 568 MB/s; VMX stores with `dcbt` 8 lines ahead 1485 MB/s.
- **Into cached memory, flushed too:** newlib's `memcpy` 410 MB/s; VMX with `dcbt` 1823 MB/s; VMX, `dcbt` and `dcbz`
  2274 MB/s.
- So aligned 64-bit and VMX stores work in write-combined memory, and VMX with `dcbt` copied fastest into it.
- **Reading the source from memory bounds a copy:** in-order 32-bit stores of values the CPU made wrote 3603 MB/s,
  where copying with the same stores ran at 517 MB/s.
- **VMX runs in titles,** as it already did in the C++ unwinder, which saves vector registers with `stvx` when a test
  throws.

## The SDK's memcpy, milestone 20 (2026-10-11)

Three runs at xsdk `bf425b9` and vulkan4xenon `109487a` (logs: `results/2026-10-11-milestone-20-memcpy/`, with the
tails the user sent for the two test titles):
- **vkbench** (`vkbench.log`): all 14 checks passed, both backends drawing the same picture in every scene. Every copy
  in the title went through the SDK's `memcpy` (`src/copy.c`), and its probes timed it:
  - into write-combined memory: 5577 MB/s for 256 KB from the cache, and 1476 MB/s for 1 MB from memory, where
    newlib's managed 185 MB/s;
  - into cached memory: 1814 MB/s for 1 MB from memory, where newlib's managed 410 MB/s;
  - prefetching 16 lines ahead copied at 1508 MB/s and 32 lines at 1315 MB/s, against 1470 MB/s for 8, so `memcpy`
    stays at 8;
  - `stream_8000` drew and submitted in 526 us through xsdk, from 5,553, and in 539 us through vulkan4xenon, from
    5,562; `stream_1000` in 32 us, from 698. The other scenes stayed within 3%.
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`): all 201 checks passed and the title exited with status 0. Among
  them were the four copy checks: every size from 0 to 300 at every source and destination alignment, 1 MB at four
  alignments, copies into write-combined memory, and `memmove` over itself both ways.
- **vulkan4xenon's test title** (`vulkan4xenon_test-tail.log`): all 58 checks passed and it exited with status 0.

With that, milestone 20 is done: `memcpy` writes write-combined memory at 5.6 GB/s from the cache, `stream_8000` fell
to match, and every test passes.

## Float constants go in groups of 4 (2026-10-11)

`build/probe_constants.xex`, with the steps added at `af68195` (log:
`results/2026-10-11-milestone-19-constant-groups/xsdk.log`), for milestone 19's third step. Its first 11 steps
passed as before. Then test_constants's six quads, each stage's c0 to c2 written per draw and the integers kept one
whole block, for 30 frames each:

- **In groups of 4, from c0:** every frame landed and every centre was right, as aligned type-0 packets and through
  `SET_CONSTANT` with its values aligned the same way.
- **One constant at a time** hung on the first frame, each packet aligned. The command processor had read everything
  (read pointer = write pointer = 0x1017) and `RBBM_STATUS` read busy, as in milestone 11's hang. So the lone float
  constants, at least, hung that run; whether its lone boolean and loop words would too isn't shown.
- **One at a time through `SET_CONSTANT`, and groups of 3,** didn't run after the hang.

So the SDK writes float constants in groups of 4, from a multiple of 4, rather than libxenon's 16.

## Fewer dwords a draw, milestone 19 (2026-10-11)

Three runs at xsdk `63fd069` and vulkan4xenon `bf60e98` (logs: `results/2026-10-11-milestone-19-step-3/`, with the
tails the user sent for the two test titles). Float constants went in groups of 4, and titles invalidated the GPU's
caches after writing memory, not every draw.

- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`): all 201 checks passed, and the title exited with status 0. Among
  them are the tests that reuse memory for new vertices, the pacing modes and the recorded buffers' moving bars, which
  showed stale vertices when the vertex cache wasn't invalidated (see "The splash stays up until present's first
  frame"). So every write the tests make is invalidated where it needs to be.
- **vulkan4xenon's test title** (`vulkan4xenon_test-tail.log`): all 58 checks passed, and it exited with status 0,
  invalidating all memory once a submit.
- **vkbench** (`vkbench.log` and its CSVs): all 14 checks passed, both backends drawing the same picture in every
  scene, with no waits.
  - A cube draw wrote 49.1 dwords through xsdk, from 156.4, and 87.1 through vulkan4xenon, from 200.3.
  - Drawing and submitting `draws_1000` took 626 us through xsdk, from 1,210, so 0.63 us a cube; `draws_4000` took
    2,487 us, from 4,823; `states_1000` 1,022 us, from 2,145.
  - Through vulkan4xenon, `draws_1000` took 1,503 us, from 2,878; `draws_4000` 6,218 us, from 11,752;
    `uniforms_1000` 5,309 us, 3.8 us a draw more than `draws_1000` for reading its uniform buffers.
  - In xsdk's `states` scenes the GPU now finished 0.8 ms after the submit, from 0.3: the CPU writes a texture and
    shader change faster than the GPU runs it. The frame's draw, submit and GPU time still fell, 1.8 ms from 2.4.

With that, milestone 19 is done. `draws_1000` draws and submits in 626 us through the GPU layer, 0.63 us a cube, where
the milestone asked for under 16 ms and started at 37,590 us; `draws_4000` is recorded with its counts; and every test
passes.

## Constants loaded from memory go in groups of 4 too (2026-10-11)

`build/probe_constants.xex` at `d3d50a5` (log: `results/2026-10-11-constant-loads/xsdk.log`), for vulkan4xenon's
milestone 6. Its first 13 steps passed as before; then test_constants's six quads, each stage's c0 to c2 loaded with
`PM4_LOAD_ALU_CONSTANT` from the quad's own write-combined memory (address, then type 0 and the index, then the
dwords), for 30 frames each:

- **In groups of 4, from c0:** every frame landed and every centre was right. So the command processor loads float
  constants from memory, big-endian words as the CPU wrote them.
- **In groups of 3** hung on the first frame, read pointer = write pointer = 0x1017 and `RBBM_STATUS` busy, as one
  constant at a time did written as registers.
- **One at a time through `LOAD_ALU_CONSTANT` or `SET_CONSTANT`, and groups of 3 written as registers,** didn't run.

So every route tried writes float constants in whole groups of 4: type-0 packets, `SET_CONSTANT` and
`LOAD_ALU_CONSTANT`.

## Constants loaded from memory, for titles and vulkan4xenon (2026-10-11)

Runs at xsdk `5d4bda6`, with xost at `b9b38e9` (logs: `results/2026-10-11-constant-loading/` and
vulkan4xenon:results/2026-10-11-milestone-7/):
- **`build/xsdk_test.xex`** (`xsdk_test-tail.log`): all 203 checks passed, and the title exited with status 0. Among
  them, test_constants drew its six quads for 60 frames with each stage's float constants loaded from memory through
  `xsdk_gpu_load_vertex_constants` and `xsdk_gpu_load_pixel_constants`, a group of 4 from c0 out of a 64-byte slot
  each, and every checked point was right.
- **vulkan4xenon's test title and vkbench** loaded every uniform block with them, at the registers xost now gives
  uniform blocks: whole groups of 4 to each. The title passed all 58 checks and vkbench all 14, both backends drawing
  the same pictures. vkbench's `uniforms_1000` drew and submitted through the layer in 717 us, from 2,091, with no
  uniform data read by the CPU.

So the GPU loads float constants from memory for titles, in whole groups of 4 from a multiple of 4.

## Calls between xsdk's GCC and the console's MSVC (2026-10-11)

A probe title of 36 cases, built once with the user's leave to compile its other half with the Xbox 360 SDK's own
compiler: GCC code called that MSVC code with each case and it called GCC code back, each side recording what
arrived (log: `results/2026-10-11-abi-probe/abi_probe.log`). The MSVC half stays in `local/`, out of git. The kernel
and XAM are MSVC's, so this is how they take calls from a title and make them into one. Xenia gave the same results.

| Case | GCC calls MSVC | MSVC calls GCC |
|---|---|---|
| 8 or fewer int or pointer arguments | pass | pass |
| arguments past the 8th | fail | fail |
| a `uint64_t` argument, or one returned | fail | fail |
| `char` and `short` arguments | pass | fail |
| `char` and `short` returned | pass | pass |
| a `float` and a `double` | pass | pass |
| int, float, int, double, int | fail | fail |
| 9 doubles | fail | fail |
| structs of 1, 8 and 24 bytes and of two floats, by value | fail | fail |
| an 8-byte struct returned | fail | fail |
| a 24-byte struct returned | pass | pass |
| varargs of int, `uint64_t`, double, int | fail | fail |
| r14-r31, f14-f31 and v14-v31 kept across the call | pass | fail |

So the console's code (MSVC 16.00.11886) takes each argument in one register, r3 to r10: a 64-bit integer whole, a
`float` or `double` in f1 to f13 while also taking up an integer register's place, a struct as its bytes across as
many registers as it fills. Structs of 8 and 24 bytes start at a register's high byte; a 1-byte struct sits in the
low byte (`clrlwi r10,r4,24` in the MSVC half's code); the probe has no other sizes. Arguments past the 8th take
8-byte slots from 0x50(r1), a 32-bit value in a slot's low word, so the 9th at 0x54. It returns 64-bit integers and
structs of 8 bytes or less in r3, larger structs through a pointer in r3. Whoever receives a `char` or `short`
extends it: the callee its arguments, the caller what comes back (`extsb r3,r3` after the call). It keeps r14-r31
whole, f14-f31 and v14-v31; it changed CR6.

xsdk's GCC, the 32-bit SysV convention, differs in each failing case: register pairs for 64-bit integers, structs by
pointer, arguments past the 8th 4 bytes apart from 8(r1), doubles in f1 to f8 only, 8-byte structs back in r3 and r4,
callers that extend, and r14-r31 kept by their low halves only, v20-v31 only. In real MSVC code, 3 of 12 `uint64_t`
values kept across a GCC call came back wrong. Layout agreed for doubles, 64-bit integers, padding and a 3:7:22
bitfield, but not `wchar_t` (2 bytes in MSVC, 4 in GCC) or a struct mixing `u8` and `u32` bitfields (12 bytes in
MSVC, 4 in GCC). r2 was 0 and r13 0x801B0000, the processor control region, before and after each side's code.

What xsdk did about it, until milestone 22 had GCC call as MSVC does ("Calls between GCC and MSVC, with GCC calling as
MSVC does"):
- **Imports:** the stubs pass r3 to r10 through and move arguments past the 8th into the kernel's slots, so imports
  take only integers and pointers of 32 bits or less. `gen_imports.py` refuses a declaration with a 64-bit integer,
  floating point or a struct by value, until a stub converts them. An import that takes UTF-16 text takes `uint16_t`,
  not `wchar_t`; none does yet.
- **The kernel calling a title:** `XSDK_KERNEL_ENTRY` saves r14-r31 whole around the title's code, and the toolchain
  keeps GCC off v14-v19 (`-ffixed-v14` to `-ffixed-v19`).

## The kernel's calls into a title keep its registers (2026-10-11)

`build/xsdk_test.xex` at `c806a87`, built with the toolchain that leaves v14-v19 alone (log:
`results/2026-10-11-kernel-entry/xsdk_test.log`): all 210 checks passed, and the title exited with status 0.
- **XSDK_KERNEL_ENTRY:** GCC code that keeps 64-bit values across calls and twenty vectors live, called directly,
  changed the high halves of r27-r31; called through the trampoline, r14-r31 came back whole, and f14-f31 and v14-v31
  unchanged. GCC's code touched none of v14-v19.
- **The audio callback,** which the kernel calls through the trampoline, ran 376 times in 2000 ms and played its tone,
  as before.
- **Threads' stacks:** a thread's stack, read from its processor control region, is 1 MB by default
  (`ExCreateThread` with a size of 0); asked for 384 KB through pthread attributes, it is 393216 bytes.

## The time base runs at 49.875 MHz (2026-10-11)

SDL's check title (`test/xsdk/sdl_check.c` in the SDL fork; logs: `results/2026-10-11-sdl-check/xsdk.log`, then
`results/2026-10-11-sdl-check-2/xsdk.log`) found three things wrong with the SDK's clocks:
- **`CLOCK_MONOTONIC_RAW` was refused.** SDL's timer reads it wherever the C library defines it, and fell back to
  `gettimeofday`, the kernel's system time: `SDL_Delay(100)` took 101000 us and `SDL_DelayPrecise(16 ms)` 16000 us,
  whole milliseconds, and every check passed. The SDK now reads the raw clock from the time base, and the title checks
  that SDL's clock steps finer than 0.1 ms.
- **The time base runs 0.25% slower than the kernel reports.** Read from the time base, the second run's
  `SDL_Delay(100)` took 99774 us. `KeQueryPerformanceFrequency` gives 50000000 Hz
  (`results/2026-10-10-probe-vblank/xsdk.log`), and at that rate the 60 Hz vblank there lasts 16624 us (930 + 15694),
  and vkbench's frames, 20 scenes of 600 paced to it, 16625 to 16627 us
  (vulkan4xenon:results/2026-10-11-milestone-8-reverted/vkbench_summary.csv). 16667 us times 49.875/50 is 16625 us:
  the time base runs at the CPU's 3192 MHz over 64, as libxenon has it (libxenon:libxenon/drivers/ppc/timebase.h:6),
  while the display and the kernel's sleeps keep true time. The SDK's monotonic clocks now count at 49875000 Hz. Times
  logged before read 0.25% short: those 16625 us frames are 16667 us. Xenia runs its time base at the 50 MHz the kernel
  reports (xenia:src/xenia/emulator.cc:151), so there the clocks run 0.25% fast, and the check that they keep the
  kernel's time fails.
- **The kernel's sleep ends early.** At the true rate, the 250 ms `sleep_for` of earlier runs, logged as 248 to 250 ms
  (248 in `results/2026-10-10-milestone-12/xsdk.log`, 249 or 250 in most), took 248.6 to 251.6 ms: a relative
  `KeDelayExecutionThread` can end up to 1.4 ms before the time asked for, which fits a wait counted from the kernel's
  last clock tick. `nanosleep`, and so `sleep_for` and `SDL_Delay`, now waits again until `CLOCK_MONOTONIC` passes its
  deadline.

## SDL's drivers and the fixed clocks on the console (2026-10-11)

`sdl_check.xex` from the SDL fork at `3d6afe95b` and `build/xsdk_test.xex`, both on the SDK at `ac1747b` (logs:
`results/2026-10-11-sdl-check-3/xsdk.log` and `results/2026-10-11-clocks/xsdk_test.log`): all 16 and all 215 checks
passed, and both titles exited with status 0.
- **SDL:** the platform reads "Xbox 360". `game:\` is the base path, and `game:\xsdk\sdl_check\` was made and a file
  written there read back. A thread ran. The controller showed up as an Xbox 360 controller, its buttons and sticks
  read, and both motors rumbled. Audio opened at 6 channels of big-endian floats at 48 kHz and played a 750 Hz tone.
- **The clocks:** `CLOCK_MONOTONIC` kept the kernel's system time to 7 ppm over 252 ms, where it was 2500 ppm off, and
  the system time stepped 1 ms at a time. Twenty 2 ms `nanosleep`s took 2.05 to 3.00 ms, none early, and
  `SDL_Delay(100)` 100016 us, with SDL's clock stepping 221 ns. Over the 2000 ms tone SDL pulled exactly 96000 frames,
  where it pulled 95744 while the clock ran slow.
- **Rumble:** `XamInputSetState` ran controller 1's motors at half strength for 500 ms and stopped them.
- **Pacing's** longest wait for a buffer reads 17995 us where it read 17949 us: the same wait, at the true rate.

## The console's calling convention, register by register (2026-10-12)

Milestone 22's first step: a title of 195 checks drove the Xbox 360 SDK compiler's own code from both ends, so what
it measured is MSVC's convention, not GCC's (logs: `results/2026-10-12-convention-model/abi_model.log`, then with the
stack below r1 measured, `results/2026-10-12-convention-model-2/abi_model.log`, whose file was cut short in copying
and whose end comes from the title's debug output). MSVC's caller of each case called a stub that recorded r3-r10,
f1-f13 and 16 stack slots. That image went to MSVC's callee of the same prototype twice: whole, and with only the
registers and slots below named, garbage in all the rest. Each time the callee read what the caller meant, and
returns went the other way. All 195 checks passed in both runs, as in Xenia.
- **Arguments:** one 8-byte slot each, r3 to r10 and then the stack from 0x50(r1), each in the next register, with no
  padding for alignment, even before 8- and 16-byte-aligned structs. The cases here all fitted in r3 to r10; past
  them, a struct of more than 8 bytes starts on an even slot of the stack (corrected in "Calls between GCC and MSVC,
  with GCC calling as MSVC does").
  - A 32-bit value goes in its slot's low word, and the high half carries nothing: MSVC's caller zero-extended an
    `int` of -5 to `00000000FFFFFFFB`, and its callee widened arguments with garbage high halves right itself.
  - 64-bit integers go whole.
  - `float` and `double` go in f1 to f13 in turn, each also taking a slot, whose register or stack word the callee
    doesn't read. From the 14th on they go in their slots, a `float` as single precision in the slot's low word.
  - Structs go as their bytes: 1, 2 and 4 bytes right-justified like integers, every other size from its first
    slot's high byte across as many slots as it fills. Structs of floats and of doubles go the same way, never in
    FPRs.
  - Varargs each take their own slot, a `double` as its bits, past the 8th too. MSVC's caller also copies doubles
    into f1, f2 and on, but its variadic callee reads only the slots.
- **Returns:** `float` and `double` in f1, a `float` as a double. Structs of 1, 2, 4 and 8 bytes in r3, 1, 2 and 4
  right-justified, a struct of one `double` too. Every other size through a pointer the caller passes in r3, which
  comes back in r3. MSVC's caller widens a returned `int` itself; its callee returned -5 sign-extended.
- **Registers kept:** f14-f31: MSVC's caller held doubles there across a call, and lost 16 of 18 when the callee
  clobbered them. It held 18 vectors across a call in VMX128's v110-v127 rather than v14-v31, saved with
  `__savevmx_110`; GCC can't reach v64-v127. No CR field: conditions MSVC held across a call survived every field
  clobbered.
- **Frames:** MSVC's code writes LR at -8(r1) through r12 and its GPR saves under that, before `stwu`, then points r12
  under the GPR saves for `__savefpr_N` and `__savevmx_N`, which store below r12 (`addi r12,r1,-72` after saving
  r24-r31; xenia:src/xenia/cpu/xex_module.cc:1341-1455). Its callers keep nothing at 4-15(r1) across a call: a
  callee that overwrote those words broke nothing. Its callees don't write there.
- **Below r1:** the console writes there, but never within 1348 bytes of r1, with r1 16-byte aligned.
  - Callees that write nothing (an empty one, MSVC's leaf for an 8-byte struct, and MSVC's 15-double callee besides
    its own saves) saw a block from 1348 to 1792 bytes below r1 written in 1 run of 8.
  - The first run's every case showed it at 1792 bytes, each straight after a line was logged.
  - A 20 ms spin saw blocks from 1348 down to 2272 bytes in every run.
  - The blocks hold what looks like a thread's saved state: `801B0000`, the processor control region; addresses in
    the title; `24FFFF22`, like a CR value.
  - So code may use up to 1348 bytes below r1 without a frame, as MSVC's saves before `stwu` do. Xenia writes nothing
    there.

## Calls between GCC and MSVC, with GCC calling as MSVC does (2026-10-12)

Milestone 22's second step: xsdk on the `mcall-xsdk` branch, built with `-mcall-xsdk` from the toolchain patches there
into `~/xsdk-gcc-next` (logs: `results/2026-10-12-convention-compiler/`).

**Two counts.** MSVC counts the GPRs apart from the parameter area's slots. A struct of more than 8 bytes starts on an
even slot, a 16-byte boundary, whatever its own alignment; its GPRs, and those of what follows, come from the GPR
count, which that padding doesn't move. The cases of step 1 never showed it, since they fitted in r3 to r10. MSVC's code
for the model's new cases showed it (`local/abi/msvc_model.dis`), and the console confirmed it with `abi_model.log`,
whose callers sent and whose callees read each word where the two counts put it, with garbage in the rest:

| Before the struct | Struct | Its GPRs | Its slots | The int after it |
|---|---|---|---|---|
| 5 ints | 24 bytes | r8-r10, all of it | 6-8 | slot 9, 0x5C |
| 6 ints | 24 bytes | r9-r10, bytes 0-15 | 6-8 | slot 9 |
| 7 ints | 9, 12 (`u32` aligned), 16 (bytes or two doubles) | r10, bytes 0-7 | 8-9 | slot 10 |
| 7 ints | 24 bytes | r10, bytes 0-7 | 8-10 | slot 11 |
| 3 ints | 48 bytes | r6-r10, bytes 0-39 | 4-9 | slot 10 |
| 9 ints | 24 bytes | none | 10-12 | slot 13 |

A slot that holds part of a struct in a register carries nothing from the caller; the callee spills the register there.
A variadic callee spills the GPRs left into the slots after its named arguments: after an int and a 24-byte struct, r7
to r10 into slots 5 to 8, which reaches 0x50(r1), past the 64 bytes a caller always leaves. Its caller passed five
varargs, so that slot was its own; a caller that passes fewer gets 0x50(r1) written all the same. GCC now does the same
in both places, so the hazard is shared.

**The probe of 2026-10-11** (`abi_probe.log`): 36 of 36 cases passed, against 9 then. Layout agreed with `ms_struct` on
GCC's side: the struct mixing `u8` and `u32` bitfields is 12 bytes in both, with the same bytes, and `char16_t` is the 2
bytes of MSVC's `wchar_t`. MSVC's code changed CR6 across a call, which GCC no longer keeps anything in.

**Both directions** (`abi_model.log`): GCC builds the model's MSVC half too, and each case's caller and callee were
called across the two compilers. All 176 calls, structs of 1 to 24 bytes as arguments and returns, 15 doubles and 15
floats, varargs past 8 slots, widening, and every case above, delivered what MSVC's own pair delivers. Built with a GCC
that has a single count, the 10 cases whose struct is padded failed both ways, in Xenia; 2 of those 20 calls had
passed anyway until each call first scrubbed the stack below it, on values the previous call had left in the slot.

**xsdk_test** (`xsdk_test.log`): 218 of 219 checks passed.
- The kernel's way of calling a title, straight into GCC code holding 64-bit values, doubles and vectors, gave back
  r14-r31 whole, f14-f31 and v14-v31. Threads and the audio callback now take GCC code directly.
- `setjmp` and `longjmp` gave back all 54 registers after frames below them had overwritten every one.
- `KeQueryPerformanceFrequency`, declared with its 64-bit return, read 50000000.
- A C++ exception thrown through those frames gave back none of the 54: the catch's caller found every register as the
  deepest frame had left it. That run is the only one that did. With the unwinder's view of each frame logged
  (`repeats.log`), the catching frame held the original values and the catch got them. Then 400 throws a launch, half
  through the same empty catch as the failing run, kept every register in three launches: after a power cycle, straight
  after, and after abi_probe and abi_model as in the failing session, where each title also passed in full. The cause
  is unknown; the 400 throws stay in xsdk_test to catch it if it comes back.

**What xsdk does now.** A title calls an import through the loader's thunk alone, each its own symbol, and declares it
with its natural types: `imports/imports.txt` lists library, ordinal and name, and `gen_imports.py` checks no
signatures. The kernel calls a title's threads, audio callback and other callbacks directly, with no trampoline, and
the toolchain no longer keeps GCC off v14-v19.

**Mixing objects.** The assembler flags each object made under `-mcall-xsdk` (`EF_PPC_XSDK`,
tools/toolchain/binutils-xsdk.patch), and the linker refuses to put one in with an object without it, either way round
(binutils:bfd/elf32-ppc.c, `ppc_elf_merge_private_bfd_data`). The convention's own attribute, `.gnu_attribute 12`,
marks only objects that return small structs. The flag turned away the probe's harness and MSVC objects assembled by
the old toolchain, and every object in the new toolchain's libc, libstdc++ and libgcc carries it.

## What the kernel leaves a title (2026-10-12)

`build/xsdk_test.xex` at `a6b5743` and Dusklight's first-step `dusklight.xex` (Dusklight `455b3f5`, built with
`int32_t` predefined as `int` until the compiler makes it so; logs: `results/2026-10-12-memory/`, the xsdk_test one
the memory test's lines and the summary): all 222 checks passed, and both titles exited with status 0.
- **`MmQueryStatistics`** answers with the 104-byte result Xenia lays out
  (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:451-473) and refuses one 4 bytes short. Pages are 4 KB: 131072
  of them, 512 MB, the highest 0x1FFFF.
- **The pages add up:** the kernel holds 784 (3 MB) and the system 7408 (29 MB, 1503 of them free), which leaves the
  title 122880 pages, 480 MB, used and free together. xsdk_test, with 1964 in use, had 120916 free (472 MB); Dusklight
  442 MB.
- **The image** counts whole, in elf2xex's 64 KB pages: xsdk_test's 656 pages, and Dusklight's 8272, its 517 64 KB pages
  (33882112 bytes).
- **Heap growth takes free pages:** 16 MB through `sbrk`, every page touched, took 4100 pages, 16 MB and 4 more.
- **Address space:** the title has 767 MB, and `sbrk`'s 256 MB reservation shows as 256 MB reserved.
- **The rest of the title's pages,** the same in both: 256 stack (the main thread's 1 MB), 96 heap, 920 physical. The
  physical pages are taken before the title's first `malloc`, so not by the splash (see the next entry); what holds
  them isn't known. Dusklight's static constructors took 256 KB of newlib's heap.
- **`pread`** read at an offset without moving the position and read nothing at the end, and **`flockfile`** kept a
  second thread waiting while `stdout` was locked twice, then once.

## The heap reserves the title's free memory (2026-10-12)

`build/xsdk_test.xex` at `70e79b6` and Dusklight's first-step `dusklight.xex` relinked with it (logs:
`results/2026-10-12-heap-reservation/`, the xsdk_test one the reservation, the memory lines and the summary): all 223
checks passed, and both titles exited with status 0.
- **At the heap's first growth,** before `main`, `sbrk` reserved what `MmQueryStatistics` gave as the title's free
  memory: 472 MB in xsdk_test and 442 MB in Dusklight, after its 32 MB image. The kernel then shows 472 MB reserved.
- **The reservation covers what's free later:** the memory test found 120916 pages free, the 472 MB of the first
  growth, after the splash had run, and Dusklight 442 MB at `main`, as at its first growth. So the 920 physical pages
  both titles hold were taken before the first `malloc`, not by the splash.

## Dusklight's first step on the console's calling convention (2026-10-12)

`build/xsdk_test.xex` at `4302e7c`, this branch with main's `-mcall-xsdk` toolchain merged, and Dusklight's first-step
`dusklight.xex` (Dusklight `455b3f5`, SDL-xenon `c86389c89`), every object rebuilt from scratch with that toolchain and
no source changed (logs: `results/2026-10-12-dusklight-mcall-xsdk/`, the xsdk_test one its summary): all 228 checks
passed, and both titles exited with status 0.
- **Dusklight** started with the whole game linked, its heap reserving 443 MB at its first growth, and had 442 MB free
  at `main`, as on the old toolchain.
- **Its image** is 8192 pages, exactly 32 MB: 80 pages smaller than the old toolchain's, and again the image elf2xex
  packed, the 33570816-byte `.xex` less its 16 KB of headers.

## The switch to the console's calling convention (2026-10-12)

Milestone 22's third step: `~/xsdk-gcc` became the `-mcall-xsdk` toolchain, built from xsdk `8a349aa`, with the SysV
one kept at `~/xsdk-gcc-sysv`. Everything was rebuilt from clean on it and run on the console:
- **sdl_check** (`results/2026-10-12-switch/sdl_check-tail.log`), from the SDL fork at `c86389c89`, where the kernel
  now calls the audio callback directly: all 16 checks passed.
- **vulkan4xenon's test title** and **vkbench** (vulkan4xenon:results/2026-10-12-mcall-xsdk/), with titles no longer
  linking `crt0.o`: all 58 and all 14 checks passed. Against the run before the clock fix
  (vulkan4xenon:results/2026-10-11-milestone-8-reverted/), its times taken as 0.25% short:
  - `stream_8000`'s simulation took 1592 us through xsdk, from 1853, and 1596 through vulkan4xenon, from 1856.
  - xsdk's own recording ran 2 to 3% faster in every draw and state scene.
  - vulkan4xenon's recording ran 10.5 to 12% slower in the draw and state scenes, and its submit about 1% faster, so
    draws and submits together took 0.5 to 1.7% longer. Two runs of the same code before differed by up to 5% in
    recording. The layer's recording functions are the same instructions under both compilers; each non-leaf frame
    now reserves the 64-byte parameter area, so `vkCmdDraw`'s grew from 32 to 96 bytes. The cause is open (TODO.md).
  - GPU times moved within the 8% two runs of the same code differed by.
  - Frames read 16667 us, the true 60 Hz; vkbench prints that as 59.9 fps, cutting off to tenths.
- **Dusklight** ("Dusklight's first step on the console's calling convention", above): its first step passed
  again, its sources unchanged; `int32_t` as `int` cleared the type errors zlib-ng, imgui's FreeType glue, zstd's
  legacy decoders, Aurora and Dusklight had.

With that, milestone 22 is done: xsdk's GCC calls as the console's own compiler does, by default, and every title
built with it passes on the console.

## Dusklight runs up to the disc (2026-10-12)

`build/xsdk_test.xex` at `f770193` and Dusklight's second-step `dusklight.xex` (Dusklight `13316c3`, Aurora `c7f5905`,
SDL-xenon `c86389c89`) linked with it, run with no disc image beside it (logs: `results/2026-10-12-dusklight-step-2/`,
the xsdk_test one its summary): all 230 checks passed and xsdk_test exited with status 0. Dusklight ended with status
1, as it does without a disc.
- **Dusklight's own startup ran** on Aurora's null backend. It read its config (none yet, so the defaults), started
  Aurora with no window, and loaded RmlUi's eight font faces from `res\`. It then found no `game:\dusklight.iso`
  (0xC0000034, `STATUS_OBJECT_NAME_NOT_FOUND`; xenia:src/xenia/xbox.h:60) and ended there, since the game reads the
  disc's ID before `OSInit`.
- **`main` gets argc 1 and an empty `argv[0]`** (`6ea8d10`), which xsdk_test checks; Dusklight reads its options from
  `argv[1]` on.
- **Paths that `std::filesystem` makes absolute reach the file** (`f770193`). libstdc++ takes `game:\TwilitRealm\Dusk`
  for a relative path and puts the current directory, `game:\`, in front of it. xsdk turned that into
  `game:\game:\TwilitRealm\Dusk`, which the kernel refused (0xC0000033, `STATUS_OBJECT_NAME_INVALID`;
  xenia:src/xenia/xbox.h:59). The run before the fix (`dusklight-before.log`: the same Dusklight, its `Build:` line
  naming `4559590`, the commit `13316c3` amended, linked with xsdk `6ea8d10`) logged 16 such `stat` failures, from
  borealis comparing its data folders through `weakly_canonical` (dusklight:extern/borealis/src/data.cpp:34-56). xsdk
  now starts a path over at a drive anywhere in it: this run logged none, and xsdk_test reads a file back through
  `absolute()` and `weakly_canonical()`.
- **Left as they are:**
  - The `mkdir` line is a folder that was already there (0xC0000035, `STATUS_OBJECT_NAME_COLLISION`;
    xenia:src/xenia/xbox.h:61).
  - The pipeline cache opens no database, since sqlite here has no VFS, and says "out of memory".
  - The window keeps its title, with no video subsystem.
  - The log carries Dusklight's colour codes.

## Dusklight runs the title scene (2026-10-12)

vulkan4xenon's milestone 9, step 3: Dusklight at `0069f0d` (Aurora `94c1353`, SDL-xenon `c86389c89`), linked with xsdk
at `c98ceb8`, with Twilight Princess's GameCube disc (GZ2E01) beside it as `game:\dusklight.iso` (logs:
`results/2026-10-12-dusklight-step-3/`, each the part of the log that was sent). xsdk hasn't changed since its 230 of
230 above.
- **The title scene ran its minute** on Aurora's null backend, and the title exited with status 0 (`dusklight.log`).
  The game logs that minute only once stage F_SP102 has started. The run before the last fix
  (`dusklight-shutdown.log`), the same game up to its shutdown, gives the frame rate: 19.1 fps over the first 10
  seconds, as the scene loaded, then 29.6 to 30.0, as in Xenia.
- **At their peaks,** the game's root heap held 140176 KB of its 262127 and newlib's heap 385684 KB, 377 MB of the
  443 MB it reserves. The game's own heaps peaked as in Xenia, to the KB.
- **A float load from the command stream raised an alignment exception** (`dusklight-float-load.log`): 0x80000002 at
  an `lfsx` in Aurora's `handle_aurora`, loading a viewport's floats, which Aurora's stream packs at any byte. The
  integer loads from the same stream before it ran. GCC had merged an integer load and a `std::bit_cast` to float into
  that one float load, as it does where nothing swaps between them; Aurora `94c1353` keeps it an integer load. Xenia
  doesn't check alignment, so it ran there.
- **A mutex relocked after its memory is freed corrupts the heap** (`dusklight-shutdown.log`): an xsdk mutex is a
  pointer to a critical section allocated on first use, and once its memory is freed that word is newlib's free-list
  link, which a lock then takes for the critical section. Dusklight's `OSResetSystem` woke the threads waiting on its
  message queues and freed the queues, mutexes included, before those threads relocked them. The title then crashed
  in `_free_r` (newlib's `_mallocr.c:2723`) with a write to 0xC, an unlink through a null link. With the queues kept
  (Dusklight `0069f0d`) it exited cleanly. Xenia never showed it.

## Floats at any address, milestone 23 (2026-10-12)

`~/xsdk-gcc` became milestone 23's toolchain, built from tools/toolchain/gcc-xsdk.patch at xsdk `e2a48fb`, with
milestone 22's kept at `~/xsdk-gcc-m22`. On xsdk's target GCC now moves a `float` or `double` between a float register
and memory only where it knows the memory is aligned to the value's size, as the console requires ("Dusklight runs the
title scene", above); any other goes through the integer registers, and an unaligned read reaches its float register
through the stack (`lwz`, `stw`, `lfs`). `-mcheck-fpr-alignment` makes any other float register access a compile
error. Logs: `results/2026-10-12-milestone-23/` and vulkan4xenon:results/2026-10-12-milestone-23/.
- **The check found none** in xsdk, SDL-xenon (`c86389c89`), vulkan4xenon (`a863279`), Aurora (`037ac39`) or
  Dusklight (`43b8f65`), every object of each compiled with it.
- **xsdk_test passed 231 of 231** (`xsdk_test.log`), among them its new check: floats and doubles written at every
  offset from 0 to 7 and read back through `memcpy`, `std::bit_cast` and packed structs, 76 reads, none wrong.
- **Built with milestone 22's compiler, the same test stopped there** (`xsdk_test_unfixed.log`): the log ends at the
  check's first line, and nothing after it ran. That build has 14 float register loads and stores on the test's
  buffer, the fixed build none; offset 0 is aligned, so the first to fault is offset 1's `lfs f1,0(r3)` in
  `read_float_memcpy`. The exception itself wasn't captured.
- **sdl_check** passed 16 of 16 (`sdl_check.log`), **vulkan4xenon_test** 64 of 64 and **vkbench** 14 of 14
  (vulkan4xenon:results/2026-10-12-milestone-23/). Against milestone 22's vkbench run
  (vulkan4xenon:results/2026-10-12-mcall-xsdk/), on the same clock: the layer's draws and submits together took 0.4 to
  0.9% less in its draw and state scenes, xsdk's own within 1.2% either way, and GPU times within 2.6%. The larger
  percentages in the summary are of values of 3 to 5 us, where 1 us of rounding reads as 20%.
- **Dusklight ran as at vulkan4xenon's milestone 10** (`dusklight.log`, against
  vulkan4xenon:results/2026-10-12-milestone-10-step-2/dusklight.log): the boot screens at 50 fps, the title scene at
  29.7 to 30.0 fps once loaded, and exit status 0, with the same unsupported-feature errors and 9 MB free at the end,
  from 7. Aurora `94c1353`'s integer read of the command stream's floats is still in, so the run shows that nothing
  else regressed; xsdk_test's `read_float_bits` is that read.

With that, milestone 23 is done: xsdk's GCC keeps float registers off memory it doesn't know is aligned, and every
title built with it passes on the console.

## Pixel shaders past 16 registers, milestone 24 (2026-10-12)

xsdk's set-up now stops stepping the shader unit's register split at the kernel's own 0x00040400, 64 registers for
pixel shaders and 64 for vertex shaders (xsdk `2562309`), where libxenon's steps go on to 0x00070100, 16 and 112
("libxenon's set-up makes the resolves work", above). The fields are Adreno 2xx's: pixel shaders' share in bits 4-11
of `SQ_GPR_MANAGEMENT`, vertex shaders' in bits 12-19 (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1231-1235).
xost builds for that split and refuses a shader past its share (xost `67994ad`), as do vulkan4xenon's builds
(vulkan4xenon `3368b84`) and Dusklight's shader pack (Aurora `82e33fa`, Dusklight `1961c31`). Logs:
`results/2026-10-12-milestone-24/` and vulkan4xenon:results/2026-10-12-milestone-24/.
- **xsdk_test passed 241 of 241** (`xsdk_test.log`). Its new checks draw with pixel shaders using 18 and 64 registers
  and a vertex shader using 64, each filling every register with a constant of its own and adding the square of how far
  each is off into its colour (tools/gen_register_shaders.py): all three quads held their colours in every one of the
  59 frames checked.
- **Under libxenon's split the 18-register draw hung the GPU** (`xsdk_test_libxenon_split.log`): its fence never
  landed in the first frame, and the GPU hadn't finished at close (0xC00000B5, so xsdk kept it and its memory); the
  231 checks before it passed. That is xost's "r16 hangs, r15 runs"
  (xost:xasm/program.py), from the split.
- **Broken on purpose,** the 18-register shader writing r17's fill into r16, as two registers sharing would, washed
  that quad to white in every frame, FFFFFFFF where 208020FF was due, and passed the other two
  (`xsdk_test_alias.log`).
- **vulkan4xenon_test** passed 64 of 64 and **vkbench** 14 of 14 (vulkan4xenon:results/2026-10-12-milestone-24/).
  Against milestone 23's run the CPU's draw and submit times moved within 1.4%, and most GPU times within 2% though
  vertex shaders went from 112 registers to 64; the state scenes' GPU times fell, xsdk's `states_250` from 793 to 591 us and
  `states_1000` from 842 to 665, and vulkan4xenon's `states_250` from 608 to 420, while vulkan4xenon's `states_1000`
  stayed at 305. Why the split helps those three and not the fourth isn't looked into.
- **Dusklight** (`dusklight.log`), with its pack's 69th program, the 18-register `e3fba406929fde97.frag`, ran the boot
  screens and the title scene as at milestone 23 and exited with status 0; the backend no longer reports that program
  missing from the pack, and its other errors are milestone 23's.

With that, milestone 24 is done: pixel shaders get 64 registers, and every title built for it passes on the console.

## Each title's log is named after its .xex (2026-10-12)

xsdk names a title's log after the `.xex` the loader started (xsdk `7b23184`), from the path the kernel leaves in
`ExLoadedImageName`, xboxkrnl's export 0x1AF (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc:177-185). On the
console it held the path the title was launched from: `\Device\Harddisk0\Partition1\XSDK\xsdk_test.xex` and
`\Device\Harddisk0\Partition1\dusklight\dusklight.xex`, so the logs came out as `game:\xsdk_test.log` and
`game:\dusklight.log` (`results/2026-10-12-milestone-24/`).

## Reads at a0, and the pixel position (2026-10-12)

For vulkan4xenon's milestone 11, xost reads a uniform array at an index known only at run time, `MOVA` setting `a0`
and the read at `c[a0 + n]`, and a pixel shader's `gl_FragCoord.xy` from the pixel parameters (xost `4d7c297`,
`7a5e42e`). Logs: vulkan4xenon:results/2026-10-12-milestone-11-step-3/.
- **Reads at a0 work in both kinds of shader.** vulkan4xenon_test's vertex shader placed and coloured each of four
  quads by the matrix its vertices named, and a pixel shader with its index from an interpolant coloured each of ten
  bands right (`vulkan4xenon_test.log`, `vulkan4xenon_diag.log` D1). Ignoring the index on purpose stacked the quads in
  one corner and turned the bands one colour (`vulkan4xenon_test_broken.log`).
- **The execs around them keep addressing mode 0.** With the mode set to a0 (1) the same pixel shader drew black
  (`vulkan4xenon_diag.log` D1b): `const_address_register_relative` in each ALU instruction is what picks a0.
- **The pixel parameters need `SQ_CONTEXT_MISC.SC_OUTPUT_SCREEN_XY`.** With `SQ_PROGRAM_CNTL.PARAM_GEN` and
  `PARAM_GEN_POS` alone, their x read 0 at every column, whether they went in r0 or r1, and with the vertex shader
  exporting the interpolator they land in (D2, D2b, D3); with bit 1 of `SQ_CONTEXT_MISC` set too, their |xy| was the
  pixel's column and row at every sample, 0 to 1279 and 0 to 719 (D4). Xenia names the bit
  (xenia:src/xenia/gpu/registers.h:156) but doesn't say what it does.
- **vulkan4xenon_test passed 72 of 72** with both, its bands right either side of their edges.

## Shaders' microcode in shared pages, milestone 25 (2026-10-12)

xsdk loads each shader's microcode into 256-byte blocks of shared 64 KiB pages (xsdk `4bcc93f`, src/microcode.c),
the alignment libxenon gives it (libxenon:drivers/xenos/xe.c:943), where `xsdk_gpu_alloc` had given each shader a
page. Logs: `results/2026-10-12-milestone-25/` and vulkan4xenon:results/2026-10-12-milestone-25/.
- **xsdk_test passed 248 of 248** (`xsdk_test.log`). Its 200 pixel shaders, each reading a constant of its own
  (tools/gen_microcode_shaders.py), and a vertex shader took 16 of the kernel's 4 KB pages, one shared page, where a
  page each took 3,216; each drew its own tile in its own colour in every frame checked, and freeing them gave the
  pages back.
- **Broken on purpose,** two shaders handed overlapping blocks, shader 98's tile drew shader 99's colour, failing only
  that check (`xsdk_test_overlap.log`); and with a page each again, the shaders took 3,216 pages, failing only the
  page check, every tile right (`xsdk_test_page_each.log`). Xenia doesn't count the pages: the check passes there
  either way.
- **IM_LOAD takes microcode from 256-byte blocks:** every shader drew right from its block, whatever block of the page
  it had.
- **vulkan4xenon_test** passed 72 of 72 and **vkbench** 14 of 14, with the same command stream as milestone 24's run:
  the CPU's draw and submit times within 1%, the GPU's within 3.5%, most of them lower.
- **Dusklight's title scene** (`dusklight.log`, saved from xbWatson, as the console's copy was cut short) ended with
  13 MB free, from 9 at vulkan4xenon's milestone 11, step 2, the title holding 69 MB of physical pages from 73; it ran
  at 29.2 to 29.8 fps once loaded, as at step 2, and exited with status 0.

With that, milestone 25 is done: shaders' microcode shares pages, and every title built with it passes on the console.

## Threads on all six hardware threads, milestone 26 (2026-10-13)

A thread xsdk starts unplaced runs on its creator's hardware thread. A probe title found it before the milestone
(`results/2026-10-13-threads-probe/`, its source and logs, built from a copy of xsdk with `probe_threads.c` and the
patches there): the main thread ran on hardware thread 0, and so did every thread it started. Alone, a busy thread
counted 31 million loop passes in 500 ms; two unplaced threads got 51% and 48% of that each, and six 16% to 20%.
`KeSetAffinityThread` moved a running thread where asked (status 0, the previous mask 0x01), and `ExCreateThread`'s
processor bits started one where asked, on 4 (`probe_threadflags.log`, the same probe placing threads only that way).
Two busy threads on two cores got 100% and 93%, on one core's two hardware threads 51% each, and six placed one to a
hardware thread 44% to 51%. A busy thread on hardware thread 2 got 93% beside one on 0, and one alone on 4 got 87%,
so something of the system's seems to run on those cores. xsdk now places threads both ways (xsdk `91075c2`,
src/thread.c). Logs:
`results/2026-10-13-milestone-26/` and vulkan4xenon:results/2026-10-13-milestone-26/.
- **xsdk_test passed 254 of 254** (`xsdk_test.log`). Threads started on 0 to 5 ran on 0 to 5, a thread moved to each
  ran there, and an unplaced one ran on 0 with the main thread. One busy thread counted 18.6 million passes in 300 ms;
  two on hardware threads 0 and 2 got 100% and 93% of that, and two on 0 got 51% and 53%.
- **Broken on purpose,** threads asked for on a hardware thread started without the processor bits all ran on 0, and
  the two busy ones got 53% and 52%, failing the start and two-core checks (`xsdk_test_created_unplaced.log`); and with
  the move kept where the thread was, only the move check failed, the thread staying on 0
  (`xsdk_test_never_moved.log`).
- **vulkan4xenon_test** passed 72 of 72 and **vkbench** 14 of 14, its frame times as at milestone 25 and its CPU times
  within 4%; neither places a thread.
- **Dusklight** (`dusklight.log`; Aurora `afb5062`, Dusklight `b0221ad`) logged each placed thread where the list puts
  it: the main thread on 0; the GPU waiter and the game's six threads on 1; the render worker on 2; both pipeline
  compiles and texture preconversion on 3; the FIFO processor on 4; the decode helper on 5. Its title scene ran at
  16.2, 19.9, 24.4, 20.0, 20.4 and 27.6 fps over its 10-second windows, against 16.9 to 22.1 with every thread on 0
  (vulkan4xenon:results/2026-10-13-milestone-11-step-5/dusklight.log), and exited with status 0, 10 MB free as then.
  Twenty frames a second is three vblanks a frame: most frames still take longer than the 33 ms of two, and what holds
  them there is measured next.

With that, milestone 26 is done: a title places its threads on the six hardware threads, and every title built with it
passes on the console.

## The log, read while it's open, milestone 27 (2026-10-13)

xsdk opened a title's log with no share access, and on the console a second open of it, for reading while the title
ran, failed with error 5, access denied. Opened shared for reading (src/log.c), it can be followed as the title writes
it. Xenia ignores share access (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:102-145), so only the console tells the
two apart. Logs: `results/2026-10-13-milestone-27/`.
- **xsdk_test passed 255 of 255** (`xsdk_test.log`). It opened its own log for reading while it was open, status 0,
  and read its first line's "log: ".
- **Broken on purpose,** with the log opened unshared, the second open failed with 0xC0000043, a sharing violation, and
  that check failed (`xsdk_test_log_unshared.log`). The controller was disconnected for that run, so its two checks
  failed as well: `XamInputGetState` returned 0x48F, device not connected.
- **vulkan4xenon_test** passed 72 of 72 and **vkbench** 14 of 14.

With that, milestone 27 is done: a title's log can be read while the title runs, and every title built with it passes
on the console.

## 32-bit float textures, milestone 28 (2026-10-13)

`XSDK_FORMAT_R32F` is the TextureFormat `k_32_FLOAT` (xenia:src/xenia/gpu/xenos.h:512), its 32-bit units swapped
8-in-32 like the other 32-bit formats. On the console the GPU read the floats xsdk_test wrote, 0 to 1 and past both
ends, linear and tiled. Logs: `results/2026-10-13-milestone-28/`.
- **xsdk_test passed 255 of 255** (`xsdk_test.log`). R32F's largest error in a channel was 0, linear and tiled, with
  every other format's as before.
- **Broken on purpose,** with R32F's units left unswapped, 4056 of the 4356 pixels of each of R32F's two cells were
  wrong, black: the floats' bytes read in reverse come out next to 0, and only the formats check failed
  (`xsdk_test_r32f_unswapped.log`). The controller had gone idle for that run, so its two checks failed as well:
  `XamInputGetState` returned 0x48F, device not connected.
- **vulkan4xenon_test** passed 72 of 72 and **vkbench** 14 of 14.

With that, milestone 28 is done: a title can sample 32-bit float textures, and every title built with it passes on the
console.

## A 2x1 texture's tail (2026-10-13)

vulkan4xenon's milestone 12 test draws a 2x1 R8 image, texels 00 and C8, filtered linearly between them. On the
console the quad's left end read 80 and its right end 60, neither texel, and that check failed (log:
`results/2026-10-13-milestone-29/vulkan4xenon_test_2x1_as_wide.log`). Such a texture's base is in its packed tail, and
xsdk, after Xenia (xenia:src/xenia/gpu/texture_util.cc:116-207), put a 2x1 base at (0, 16), where a wide texture's
goes.

probe_mip_tail, run with eleven shapes one or two texels high or wide in place of its own, found where the console
reads each (log: `probe_mip_tail_thin.log`). Every one read one rectangle of the base's page, with the mip address on
its own page or on the base's.
- **2x1 at (16, 0),** as a square's base, and **1x2 at (16, 0),** as a tall one's.
- **4x1, 8x1, 16x1 and 32x1 at (0, 16),** as a wide one's, and so 4x2, 16x2, 16x4 and 16x8; **1x16 at (16, 0).**
- So one texel high counts as two when the console weighs width against height for the tail. A 2x1 level further down
  a larger texture's tail, 16x8's level 3, was already where xsdk put it: there the rule takes the base's sides.

xsdk lays out a 2x1 base as the console reads it (src/texture_layout.c), and probe_mip_tail now draws 2x1, 4x1 and 1x2
among its shapes. Xenia still lays out 2x1 as wide, so a 2x1 texture with the SDK's layout reads wrong there.

## Uploads in runs, and channel swizzles, milestone 29 (2026-10-13)

`xsdk_texture_upload` writes runs of blocks that sit together in memory, one offset each and as whole words: 16 bytes
of Xenos's tiled layout (xsdk_tiled_2d), or a linear row. `xsdk_texture_swizzle` sets the fetch constant's swizzle
(xenia:src/xenia/gpu/xenos.h:1015-1030), composed with the format's own. Logs: `results/2026-10-13-milestone-29/`.
- **xsdk_test passed 257 of 257** (`xsdk_test.log`). Every format, linear and tiled, and every mipped level read as the
  CPU decodes it, as before. R8 swizzled to red in every channel and R8G8 to red, red, red then green read every pixel
  as decoded.
- **A 512x512 R8 tiled upload from cached memory took 2226 us,** 117 MB/s, against the check's 4 ms.
- **Broken on purpose,** with each block written on its own as before, it took 84730 us, 3 MB/s, and that check failed
  (`xsdk_test_one_block_runs.log`). With the swizzles left at the format's, R8 and R8G8's cells had 4096 of 4356 pixels
  wrong each, red in red alone, and only the swizzle check failed (`xsdk_test_swizzle_ignored.log`). The controller had
  gone idle for both runs, so its two checks failed as well: `XamInputGetState` returned 0x48F, device not connected.
- **probe_mip_tail** read 2x1, 4x1 and 1x2 where xsdk puts them (`probe_mip_tail.log`), and vulkan4xenon_test's 2x1 R8
  image drew its texels, filtered between them.
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14.

With that, milestone 29 is done: textures upload in runs, a title can pick a texture's channels, and every title built
with it passes on the console.

**Whole tiles at once, vulkan4xenon milestone 29 step 6 (2026-10-19).** Dusklight's attract movie spent about 2.35 ms
of every frame uploading its video planes, R8, through `xsdk_texture_upload`. `copy_rows` now puts a row's whole
32-block tiles in one call (`put_tiles`, src/texture.c): each tile's offset steps from the last, the carry skipping the
bank and pipe bits between (`xsdk_tiled_tile_bits`, src/tiled.c), and a tile's runs go at offsets fixed for its block
size. R8 writes 32-bit stores; wider blocks write VMX stores to 16-byte aligned runs. Rows that aren't word-aligned, the
blocks before a row's first whole tile and after its last, downloads and linear textures take the old path. Logs:
`results/2026-10-19-tiled-uploads/`.
- **A 512x512 R8 tiled upload took 461 us, 568 MB/s,** from 2226; a 512x512 A8R8G8B8 one 948 us, 1106 MB/s.
- **Byte for byte as before:** 11 textures of every block size, 2D, cube and 3D, every level of four, rows split in
  two and at varying alignment, widths past whole tiles, each uploaded both ways over a sentinel.
- **Broken on purpose,** the step between tiles not carrying across the bank and pipe bits, that check failed alone.
- **Dusklight's movie** uploaded its frames in 460 to 544 us a frame, and its scene's whole submit took 0.61 to 0.69 ms.

## Multisampled targets, milestone 32 (2026-10-13)

`xsdk_gpu_set_target_samples` and `xsdk_gpu_set_target_samples_at` take 1, 2 or 4 samples a pixel, and
`xsdk_gpu_target_sample_tiles` counts their eDRAM tiles; the three calls without samples take 1, so vulkan4xenon builds
as it is. `RB_SURFACE_INFO` holds the count (xenia:src/xenia/gpu/registers.h:670-679), a pixel's 2 samples take two
rows of eDRAM and its 4 a square of two by two (xenia:src/xenia/gpu/shaders/edram.xesli:27-32), and a resolve averages
them with `RB_COPY_CONTROL`'s sample select, 4 for samples 0 and 1 and 6 for 0 to 3 (registers.h:858; xenos.h:1000-1008;
libxenon:drivers/xenos/xe.c:1096-1105). Logs: `results/2026-10-13-milestone-32/`.

A probe settled the rest before the test (`probe_msaa.c`, built from xsdk at the milestone's code commit with
`xsdk-probe.patch`, which puts back libxenon's `PA_SC_AA_CONFIG` values, as xsdk had them then, and lets the probe set
the resolve's; `probe_msaa.log`). Each case cleared a 256x128 target with a resolve, drew a triangle, resolved, and
counted the pixels near its edges at each blend of its colour and the clear colour:
- **`PA_SC_AA_CONFIG` takes the samples less one.** Its fields are the count and, from bit 13, the farthest sample from
  a pixel's centre in 16ths of a pixel (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1540-1543). With 0x8001
  at 2 samples and 0xC003 at 4, the value XAM leaves ("A resolve needs multisampling off"), edges blended: of 1290
  pixels near them, 247 half way at 2 samples, and 118, 138 and 116 a quarter, half and three quarters of the way at 4.
  Every pixel off the edges came out as at 1 sample. libxenon's 0x8002 and 0xC004 (xe.c:1738-1740), which it never
  runs (xe.c:1004), fail: at 2 samples no pixel blended, the edges as at 1 sample; at 4 the resolve wrote every other 4
  pixels of a row and left half the pixels unwritten.
- **A resolve needs the target's value too.** With 0 there, a 2-sample resolve wrote every pixel wrong, the first ones
  0, and a 4-sample one left half the pixels unwritten.
- **Draws need `PA_SU_SC_MODE_CNTL`'s msaa_enable** (registers.h:474), which libxenon sets with samples
  (xe.c:1411-1412): without it, at 2 and 4 samples alike, the edges came out as at 1 sample.
- **Where the samples sit.** An edge rising one pixel over 256 columns, and one moving a pixel over 128 rows, give each
  sample's offset down and across. 1 sample sits at the centre, as the half-pixel edges assume
  (xenia:src/xenia/gpu/draw_util.cc:837); 2 at a quarter of a pixel either side of it, down and across; 4 at an eighth
  and three eighths either side, each at an offset of its own down and across. The farthest, 4/16 and 6/16 of a pixel
  each way, are the farthest-sample fields of 0x8001 and 0xC003. Which offsets pair up into one sample, and which
  sample is which, the sweeps don't show; Xenia takes sample 0 of 2 for the top one (draw_util.h:317-320).

Then, with xsdk setting 0x8001 and 0xC003:
- **xsdk_test passed 267 of 269** (`xsdk_test.log`), the two failures the controller's, none connected:
  `XamInputGetState` returned 0x48F. Over four rounds, a triangle with a shallow, a steep and a diagonal edge, drawn
  at 1, 2 and 4 samples into a 240x128 target, in xsdk's layout and placed as vulkan4xenon places targets, with raster
  state set after the target, resolved into a tiled A8R8G8B8 texture. Counted over the rounds, the pixels near the
  edges held the clear colour 2844 times at 1 sample and the triangle's 2032; at 2, the clear colour 2368 times, half
  way 956 and the triangle's 1552; at 4, 2132 times, then a quarter, half and three quarters of the way 464, 500 and
  452, and the triangle's 1328. No pixel near the edges held anything else, and every pixel off them was as the CPU
  works it out, placed or not. The 4-sample target, 240 pixels wide, counts 8 tiles across as its texture's
  256-texel pitch does, so the resolve across the pitch read it where it was drawn.
- **Broken on purpose,** the resolve copying sample 0, the pixels near the edges at 2 and 4 samples were all one
  colour or the other, 2836 and 2040, and the four checks at 2 and 4 samples failed, besides the controller's
  (`xsdk_test_one_sample.log`).
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14, its xsdk scenes' CPU and GPU times within 3% of
  milestone 29's. A first vkbench run's log stopped 12.8 s in, after "xsdk: opened", with no exit line, while the
  console reported no title running (`vkbench_cut_short.log`); the cause isn't known, and run again it passed.
- **A vblank missed at times this evening:** two runs of this milestone's earlier builds each missed one, in replay and
  in pacing's 2 buffers at every vblank, and so did xsdk_test at `db55e35`, before the milestone, in replay
  (`xsdk_test_before.log`). The runs recorded above missed none.

With that, milestone 32 is done: a title can draw into targets of 2 and 4 samples, resolves average them, and every
title built with it passes on the console.

## Anisotropic filtering, milestone 33 (2026-10-13)

`XsdkSampler`'s `max_anisotropy` sets the fetch constant's `aniso_filter`: 0 for none, then 1 to 5 for at most 1, 2,
4, 8 and 16 to 1 (xenia:src/xenia/gpu/xenos.h:140-148, 1259). The tests' pixel shader leaves the filter to the fetch
constant, its `tfetch` built with AnisoFilter 7 (tools/gen_texture_shaders.py; xost:xasm/encode.py:407). Logs:
`results/2026-10-13-milestone-33/`.
- **xsdk_test passed 264 of 264** (`xsdk_test.log`). It drew a tiled 256x256 texture of black and white stripes two
  texels wide, its nine levels box-filtered, with linear filters and mips, on 256x32 quads: a texel a pixel across and
  eight down. The stripes' contrast, the white stripes' average less the black ones' over 255, came to 0 with none and
  with at most 1 and 2, 0.501 with 4, and 1.000 with 8 and 16; every quad averaged 0.50.
- **So the level read is the squeeze over the most anisotropy,** as an anisotropic filter's should be. 8 over 8 or 16
  leaves level 0. Over 4 it leaves level 1, whose one-texel stripes, read a quarter texel off their centres, keep half
  their contrast. Over 2 or 1, or with none, it leaves level 2 or 3, all grey. At most 1 draws as none.
- **Broken on purpose,** with the field left out, every quad came out grey, contrast 0, and the check at 16 failed
  (`xsdk_test_aniso_left_out.log`). That run also missed one vblank in the replay test. A second run of the same build,
  with the controller disconnected, missed two in pacing's two buffers at every vblank instead
  (`xsdk_test_aniso_left_out_2.log`). The build that passed missed none, and the change touches no timing.
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14, its times as at milestone 29.

With that, milestone 33 is done: a title's samplers take up to 16 times anisotropy, and every title built with it passes
on the console.

## A clock the GPU and the CPU both read (2026-10-13)

`probe_timestamp.c`, built as a probe against milestone 33's code on `db55e35`, before milestone 32 came under it,
looked for a GPU time stamp to time fence waits by (log: `results/2026-10-13-milestone-34/probe_timestamp.log`, its
source beside it).
- **The performance counters stood at 0 as XAM left them:** `CP_PERFCOUNTER0_LOW`, `RBBM_PERFCOUNTER0_LOW` and
  `RBBM_PERFCOUNTER1_LOW` (xenia:src/xenia/gpu/register_table.inc:42-52) read 0 and didn't move in 1 ms.
- **With `CP_PERFMON_CNTL` reset and then set to 1, and the counters' selects 0,** written from the CPU or through the
  ring alike, each counted 499,747 to 499,809 in 1,001,764 ns of the CPU's clock: 498.9 a microsecond, near the GPU's
  published 500 MHz. Linux's Adreno 2xx registers name those values PERF_STATE_ENABLE, RBBM1_COUNT and ALWAYS_COUNT
  (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:719-720, 738-739, 1022-1025).
- **`PM4_REG_TO_MEM` copied each counter into memory** (xenia:src/xenia/gpu/command_processor.cc:1069-1090), the copy
  between the CPU's reads of it before the fence and after: the command processor and the CPU read the same clock.
- **`PM4_EVENT_WRITE_SHD` with its initiator's bit 31 set wrote a clock in place of the packet's value,** as Xenia has
  it (command_processor.cc:1204-1206). Two such writes differed by 6,043,369 while `RBBM_PERFCOUNTER1` moved 6,043,389
  between them, so it runs at the GPU's clock too, from another start, and no register read by the CPU was found to
  hold it. With bit 30 set instead, it wrote the packet's value.
- **Fences, timed by spinning on them:** a fence alone passed 1.3 us after its submit, a 1280x720 resolve 290 us after
  it, four resolves 1,157 us. `xsdk_gpu_wait`, sleeping 1 ms between looks, returned 1.06 to 2.56 ms after the submit
  for each of them.

## Looking at a fence costs the GPU (2026-10-13)

The first build of milestone 34 looked at the fence without a break for up to 2 ms. Its xsdk_test timed each 1280x720
resolve at 287 to 290 us by `RBBM_PERFCOUNTER1` (`results/2026-10-13-milestone-34/xsdk_test_looks_unspaced.log`),
where a wait that sleeps first, as the broken build's does, leaves it 269 us (`xsdk_test_sleep_first.log`). Every
look reads the fence from uncached memory, which the GPU shares. `probe_fence_look.c`, built against that first build,
timed 16 resolves for each way of waiting (`probe_fence_look.log`, its source beside it):

| While the CPU waits | The resolve, mean | The wait's return after the fence, at most |
|---|---|---|
| asleep, then looking once | 134,582 clocks, 269.2 us | (the sleep's) |
| looking without a break | 140,266 clocks, 4.2% longer | 1.4 us |
| looking every 1 us | 0.45% longer | 2.2 us |
| looking every 2 us | 0.27% longer | 2.9 us |
| looking every 5 us | 0.13% longer | 4.2 us |
| looking every 10 us | 0.05% longer | 11 us |
| looking every 20 or 50 us | as asleep | 17 and 33 us |

So `xsdk_gpu_wait` looks every 5 us, spinning on the time base in between.

## Fence waits, and drawing after a resolve, milestone 34 (2026-10-13)

`xsdk_gpu_wait` looks at the fence every 5 us for up to 2 ms, as long as one of its 1 ms sleeps takes (1993 us, "Sleeps,
write speeds, and a set-up that wouldn't run from a cached stream"), then sleeps as before (src/gpu.c). A draw after a
resolve, before the target was set again, hung the GPU while milestones 30 to 35 were measured (SCOPE.md, milestone
34): a resolve leaves eDRAM copying with the viewport transform off, and keeps only where `xsdk_gpu_set_target_at` put
the target. Draws now assert that the target was set since the last resolve; draws into a recording aren't checked, as
they run when their buffer does (src/gpu_draw.c). Logs: `results/2026-10-13-milestone-34/`.
- **xsdk_test passed 263 of 265** (`xsdk_test.log`), the two failures the controller's, which was disconnected
  (`XamInputGetState` returned 0x48F). Eight waits on a fence after a 1280x720 resolve returned 3.8 to 5.0 us after it
  by `RBBM_PERFCOUNTER1`, copied by `PM4_REG_TO_MEM` once the GPU was idle just before the fence and read by the CPU as
  each wait returned; the resolves took 279 us. The assert held through every test.
- **Two earlier runs of the same build missed vblanks in pacing,** one at two buffers and every second vblank, two at
  two buffers and every vblank (`xsdk_test_2.log`, `xsdk_test_3.log`); their waits returned 1.3 to 4.7 us after their
  fences, the resolves taking 271 and 279 us. The misses came with the console that evening, not with the milestones:
  xsdk_test built at `db55e35`, before milestone 33, missed two in replay (`xsdk_test_before.log`), and two of
  milestone 33's runs missed some too. Pacing's frames wait in `xsdk_present_begin`'s own sleeps, not in
  `xsdk_gpu_wait`. No recorded run before that evening missed one, and other sessions' probes ran on the console
  between these runs.
- **Broken on purpose,** sleeping 1 ms before the first look, the waits returned 317 us to 1.55 ms after their fences,
  and that check failed besides the controller's two (`xsdk_test_sleep_first.log`).
- **probe_draw_after_resolve stopped at the assert** (`probe_draw_after_resolve.log`). After drawing a triangle,
  resolving and waiting, it drew again without setting the target. Its log ended with newlib's line for the assert,
  `assertion "!target_undone || xsdk_gpu_recording()" failed`, in `xsdk_gpu_draw` at src/gpu_draw.c:263, then
  `exit: signal 6` and status 134. The draw never reached the GPU, and the console went back to Aurora.
- **vulkan4xenon_test** passed 80 of 80 with the assert in, and **vkbench** 14 of 14, its CPU times within 1% of
  milestone 33's run and its GPU times within 4%; its frames look at their fences themselves.

With that, milestone 34 is done: a fence wait returns within microseconds of a short job's fence, a draw after a
resolve stops at an assert, and every title built with them passes on the console. Why the console missed vblanks that
evening, with xsdk_test from before the milestones too, isn't known.

## Milestones 33 and 34 on top of milestone 32 (2026-10-13)

The runs above were of milestones 33 and 34 on `db55e35`. Moved onto milestone 32, each ran its checks again, built
clean at xsdk `75e17eb` and `ca0fd64` (logs: the `_on_32` ones in `results/2026-10-13-milestone-33/` and
`results/2026-10-13-milestone-34/`). The controller was disconnected throughout, so its two checks failed in every run
(`XamInputGetState` returned 0x48F).
- **Milestone 33:** xsdk_test passed 274 of 276, milestone 32's multisampling checks among them, the stripes' contrast
  0, 0, 0, 0.501, 1.000 and 1.000 as before. With the field left out, every quad came out grey and the check at 16
  failed; replay missed two vblanks in that run. vulkan4xenon_test passed 80 of 80 and vkbench 14 of 14, its times
  within 1.1% of milestone 32's run.
- **Milestone 34:** xsdk_test passed 275 of 277 and missed no vblank. Eight waits after a 1280x720 resolve returned
  1.4 to 2.4 us after their fences, the resolves taking 271 to 272 us, and the assert held through every test,
  milestone 32's multisampled draws among them. Sleeping first, the waits returned 186 us to 1.66 ms late, but for one
  whose sleep ended before its fence passed, 3.2 us, and that check failed. probe_draw_after_resolve stopped at the
  assert, now at src/gpu_draw.c:328. vulkan4xenon_test passed 80 of 80 and vkbench 14 of 14, its CPU times within 2%
  of milestone 33's run and its GPU times within 4%.

## The pixel rate, milestone 31 (2026-10-13)

Every pixel shader xsdk drew asked the scan converter for centroids and centres, `SQ_CONTEXT_MISC`'s sc_sample_cntl 2
(xenia:src/xenia/gpu/registers.h:157; xenos.h:804-808): xost writes it into every pixel shader it builds
(xost:xasm/program.py:604), xsdk's own generators do too, and `xsdk_gpu_set_shaders` passed it on. On the console that
holds the pixels to a quad a clock. xsdk now asks for centres only, whatever the shaders say (xsdk `4b137de`,
src/gpu_draw.c). No pixel changes: xsdk interpolates every interpolant at its centre (`SQ_INTERPOLATOR_CNTL`
0xFFFFFFFF, libxenon:drivers/xenos/xe.c:1400), the pattern that centroids and centres samples by
(xenia:src/xenia/gpu/xenos.h:810-831). Logs: `results/2026-10-13-milestone-31/`.

`tests/probe_pixel_rate.c` times, into a 1280x720 target: full-screen quads, opaque, blended, and depth-only with the
colour masked and with `RB_MODECONTROL`'s kDepthOnly (xenos.h:886-911); 4 point or 4 bilinear samples a pixel from a
256x256 tiled texture, a texel a pixel; 28,800 triangles of 8 pixels, listed and by index; and 1280x720 resolves.
Each batch takes about 100 ms by the time base and ends with a fence it looks at every 20 us ("Looking at a fence costs
the GPU"), and after each, a resolve checks its pixels. The fill's pixel shader writes c0 in one instruction and reads
no register; the sampled one takes its 4 samples into r1 to r4 and sums them, 5 registers (tools/gen_fill_shaders.py);
the vertex shader is tools/gen_texture_shaders.py's. A clock is the published 500 MHz. Each variant changes one piece
of xsdk's state, from Xenia, libxenon or the console's own values.
- **Centroids and centres hold the pixels to a quad a clock.** Built before the fix (`probe_pixel_rate_before.log`, an
  earlier version of the probe, on xsdk before milestones 32 to 34), opaque, blended and colour-masked depth fills all
  ran 1990 Mpixels/s, 3.98 a clock, and 8-pixel triangles by index 166 million a second, 3 quads a triangle. With
  sc_sample_cntl 1, centres only, the fills ran 3442 Mpixels/s, 6.88 a clock, and the triangles 249 million; with 0,
  centroids only, 3424, 6.85. `PA_SU_SC_MODE_CNTL`'s sc_one_quad_per_clock (registers.h:483) brought every fill to
  the same 3.98, kDepthOnly's too, under any sample control.
- **With centres only in xsdk** (`probe_pixel_rate.log`) the fills run 3442 Mpixels/s, 6.88 a clock, and the indexed
  triangles 249 million a second; every check drew right.
- **Broken on purpose,** built with `xsdk_gpu_set_shaders` passing the shaders' sample control on as before, xsdk's
  state fell back to 1990 Mpixels/s, 3.98 a clock, and 166 million triangles (`probe_pixel_rate_fix_reverted.log`).
- **Nothing else tried moved the fills past 6.91 a clock** on top of centres only, nor, of those the earlier version
  tried, from 3.98 under the old state (`probe_pixel_rate_before.log`):
  - `SQ_CONTEXT_MISC`'s inst_pred_optimize and yield_optimize (registers.h:155, 236);
  - `SQ_INTERPOLATOR_CNTL`'s param_shade 0 (registers.h:247), which also put the texture coordinates wrong, those
    checks failing, so xsdk's 0xFFFF there is right;
  - `PA_SU_SC_MODE_CNTL`'s quad_order_enable and persp_corr_dis (registers.h:479-482);
  - `RB_SURFACE_INFO` without its HiZ pitch (registers.h:676), and colour before depth in eDRAM;
  - `PA_SC_VIZ_QUERY`, `PA_SC_MPASS_PS_CNTL`, `VGT_ENHANCE`, `SQ_DEBUG_MISC_0` and `_1` and `RB_SAMPLE_COUNT_CTL`
    written 0, which xsdk never writes (xenia:src/xenia/gpu/register_table.inc:567-607);
  - `PA_CL_ENHANCE` 7, as the kernel leaves it, for libxenon's 3, and `SQ_FLOW_CONTROL` 0x04007000 for 0x04000000
    ("libxenon's set-up makes the resolves work"); read at title start in these runs, `SQ_FLOW_CONTROL` held 0x04001000,
    0x04004000 or 0x04005000, so its bits 12-15 change by themselves;
  - the register split at 16 vertex and 112 pixel registers, and at 96 and 32, stepped as libxenon steps it;
  - the fill reading its interpolant, taking its colour through a second instruction, or with its alloc unserialized
    (xenia:src/xenia/gpu/ucode.h:488);
  - each quad drawn as one rectangle with clipping off, as the resolve draws (xenos.h:53; libxenon xe.c:1140).

  So fills through a pixel shader reach 6.88 of the 8 a clock, and what holds the rest isn't found.
- **kDepthOnly ran 6711 to 6897 Mpixels/s, 13.42 to 13.79 a clock,** under every state but sc_one_quad_per_clock, its
  depth written (a quad behind it then drew nowhere): past the 8, so the pixel shader doesn't hold it, as Xenia supposes
  it isn't run there (xenos.h:888-910). xsdk doesn't use the mode.
- **4 samples a pixel ran 1990 Mpixels/s, 7960 Mtexels/s, 15.9 a clock,** point and bilinear, before the fix and after:
  the texture units' 16, with the shader's 5 registers. Declared with 12 it ran 6344 Mtexels/s, 12.7 a clock, and with
  12 and the split at 112 pixel registers 7960 again; with 5 and 32 pixel registers, 7224. So what held a sampled fill
  was threads in flight, as xost's register packing found (xost `cbf5c3e`), not the sample control.
- **Listed 8-pixel triangles ran 162 million a second** under every state, 3 vertices each, about a vertex a clock.
- **A 1280x720 resolve took 270 to 279 us, 3298 to 3416 Mpixels/s,** under every state; the earlier version of the
  probe, which looked at its fences without a break, timed them 4 to 7% longer.
- **xsdk_test passed 275 of 277** (`xsdk_test.log`), the two failures the controller's, none connected:
  `XamInputGetState` returned 0x48F. Milestone 32's edges counted as before, 2368, 956 and 1552 at 2 samples and 2132,
  464, 500, 452 and 1328 at 4. No vblank was missed.
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14 (`vulkan4xenon_test.log`, `vkbench.log`). Against
  milestone 34's run the CPU's times moved within 6%, and the GPU's fell where pixels fill the frame: `fill_16` from
  7708 to 4578 us and `fill_4` from 2153 to 1366 in both backends, `stream_8000` from 6829 to 4119 and `stream_1000`
  from 1112 to 774, `draws_250` and `states_250` to 0.70 to 0.79 of their times; the rest moved within 5%.

Dusklight's logo shot isn't run here. With that, milestone 31 is done but for it: fills through a pixel shader run
6.88 pixels a clock where they ran 3.98, small indexed triangles 249 million a second where they ran 166, and every
title built with it passes on the console; what holds the fills below 8 isn't found.

## Loop unrolling, milestone 35 (2026-10-13)

GCC already unrolls small loops at -O2 for PowerPC: from -O2 up it turns `-funroll-loops` on with
`-munroll-only-small-loops` and keeps register renaming off (gcc:common/config/rs6000/rs6000-common.cc:39-46), which
unrolls a loop of up to 6 instructions 4 times and one of up to 10 twice (config/rs6000/rs6000.cc:5569-5582).
`-funroll-loops` given on the command line unrolls every loop whose count is known on entry, up to 8 times and 200
instructions (params.opt:779-784), and renames registers (rs6000.cc:3421-3435); `-Q --help=optimizers --help=target`
on xsdk's GCC lists those differences and no others. Everything below was built both ways, every C and C++ compile of xsdk,
vulkan4xenon, Aurora, Dusklight and the libraries Dusklight builds given `-funroll-loops` or not (`build.sh`); newlib,
libstdc++ and SDL, built before, are the same in both. Logs: `results/2026-10-13-milestone-35/`.
- **probe_unroll** builds six loops into one title both ways and times each nine times, alternating
  (`probe_unroll.log`; `probe_unroll_again.log` matched it to three digits). The best of nine, in CPU clocks, 64 a
  time-base tick, at -O2 and with `-funroll-loops`:
  - 8 independent chains of double multiply-adds: 1.668 and 1.251 clocks a multiply-add, 3.83 and 5.10 GFLOPS, 1.33
    times as fast. Unrolled 4 times, the 8 chains take 10 clocks, so a dependent multiply-add takes 10; at -O2 the
    loop's count, compare and branch add 3.3 clocks to every 8.
  - FNV-1a over bytes, one chain of multiplies: 16.0 and 14.1 clocks a byte, 1.14 times.
  - A word copy: 7.52 and 6.30 clocks a word, 1.19 times at best and 1.07 at the median.
  - MurmurHash3's finalizer over keys: 41.7 and 40.1 clocks a key, 1.04 times.
  - A multiply-add over float arrays, `out[i] = a * x[i] + y[i]`, which -O2 already unrolls twice: 13.8 and 14.9 clocks
    a result, 0.92 times.
  - A 4x4 matrix times vectors: 46.8 and 49.5 clocks a vertex, 0.95 times.

  Each loop's results were the same both ways. In the two float loops that lose, each result is stored through a
  pointer that may alias the next iteration's loads, so GCC can't move those loads above the store, and unrolled
  iterations still wait on each other's 10-clock multiply-adds.
- **Dusklight's logo shot**, from the diagnostics build (`dusklight_diag_o2.log` and `dusklight_diag_unroll.log`, each
  with an `_again` run; means over phases 0 to 8, the same still shot):
  - The FIFO worker took 11.6 ms a frame at -O2, both runs, and 10.3 unrolled, both runs.
  - The render worker took 11.6 ms at -O2 and 14.2 unrolled. Its own work, besides the replay and present, went from
    2.0 ms to 4.7; the replay's CPU work, without its wait for a resolve rectangle, stayed at 0.6 ms.
  - The GPU took 14.0 ms from submit, and 14.1 unrolled; in phase 0, nothing skipped, 20.1 and 20.3 ms, and 20.4 and
    20.5.
  - The game thread never waited on Aurora either way, and the shot's last frame drew the same pixel for pixel.
- **The title scene** (`dusklight_o2.log`, `dusklight_unroll.log`) ran at 21.8, 29.8, 29.0, 30.0, 29.9 and 29.9 fps at
  -O2 and 21.7, 30.0, 28.9, 30.0, 29.9 and 29.8 unrolled; the GPU holds it near 30 either way. It ended with 122 MB free
  at -O2 and 106 unrolled: the image 3 MB larger, 9280 pages from 8496, and newlib's heap holding 257 MB in use, from
  242. The diagnostics runs ended with 119 and 116 MB free, 242 MB in use, against 101 and 104, 252 MB in use.
- **Where the difference comes from:** the diagnostics build relinked from both builds, some libraries from one and the
  rest from the other (`diagnostic/relink.sh`). With the game's own objects unrolled and every library at -O2
  (`dusklight_diag_unroll_game_only.log`), it ran as -O2 did: the FIFO worker 11.6 ms, the render worker's own work
  2.0, 242 MB in use; and so with only xsdk and vulkan4xenon unrolled (`dusklight_diag_unroll_sdk_only.log`): 11.5
  ms, 2.1 and 242 MB. With only the xxhash library unrolled (`dusklight_diag_unroll_xxhash_only.log`), it ran as the
  unrolled build did: 10.0 ms, 4.4 and 252 MB; so it did with abseil and xxhash unrolled
  (`dusklight_diag_unroll_abseil_xxhash_only.log`), and with everything unrolled but Aurora's own libraries
  (`dusklight_diag_unroll_aurora_o2.log`). Aurora hashes vertex and storage data, pipelines and bind groups with
  xxhash's XXH3 (aurora-xenon:lib/gfx/hash.hpp:18, lib/gfx/recording.cpp:894, 952). Its hashes are right unrolled: a
  title hashing the same data through each build's library logged what xxhash on the PC does (`xxh_check_o2.log`,
  `xxh_check_unroll.log`, `xxh_check_host.log`; `diagnostic/xxh_check.c`). Why its unrolled code makes the render
  worker slower and the heap fuller isn't known.
- **vkbench** (`vkbench_o2.log`, `vkbench_unroll.log`) passed 14 of 14 both ways. Unrolled, xsdk's scenes took 6 to 7%
  less CPU time a frame (draws_4000 2418 us to 2257) and vulkan4xenon's 8 to 10% less (draws_4000 5266 us to 4751,
  recording 1168 to 1094 and the replay 4102 to 3656), but its uniforms_1000 3% more (708 to 729).
- **Code** (`sizes.txt`): unrolled, Dusklight's text grew 12.3%, from 26.07 MB to 29.27 MB, and its .xex from 34.8 MB to
  38.0 MB; xsdk_test's text grew 3.1%, vulkan4xenon_test's 13.3% and vkbench's 12.3%. RmlUi grew most, by 630 KB, then
  zstd by 398 KB. The instructions Xenia skips (`tools/xenia_scan.py`) went from 539 to 558 in Dusklight and from 36 to
  50 in xsdk_test.

So the toolchain keeps GCC's -O2, and tools/toolchain/build.sh doesn't change: unrolled, Dusklight's render worker
took 2.6 ms more a frame, more than its FIFO worker saved, and it had 16 MB less free, for code 12% larger. Unrolling
paid in xsdk's and vulkan4xenon's own code, 6 to 10% of vkbench's CPU time with nothing lost in Dusklight, but not as
the default for everything a title builds.

With that, milestone 35 is done: the console's numbers keep loop unrolling at GCC's -O2 default.

## Hierarchical depth, milestone 30 (2026-10-13)

Draws that test depth now skip hidden pixels before the pixel shader, through Xenos's only early rejection, hierarchical
depth (xenia:src/xenia/gpu/xenos.h:893-894). Xenia names `RB_HIZCONTROL`'s bit 0 hiz_write_enable and bit 1 hiz_enable
(xenia-canary:src/xenia/gpu/registers.h:822-831, from its commit `692cd59`), and nothing public had more, so discovery
probes settled the rest. Each drew full-screen and smaller quads, checked every pixel against the CPU's depth test, and
read what the buffer holds back through columns at many depths, with depth itself cleared past them
(`probe_hiz_discovery*.c` and their logs; built from xsdk without the milestone's code and with `xsdk-probe.patch`,
which lets them pick the resolve's `RB_HIZCONTROL` and the extra passes below; runs 1 to 6 on `db55e35`, run 7 on
`079f061`). Logs: `results/2026-10-13-milestone-30/`.
- **Bit 0 writes the buffer and bit 1 tests it.** Eight full-screen quads through a 64 multiply-add pixel shader behind
  a near quad took 20067 us with 0 and with 1, 367 Mpixel/s as drawn ones, and 241 us with 2 and 242 with 3, 30
  Gpixel/s or 61 pixels a clock; with 2 alone they were rejected against what the run with 1 had written
  (`probe_hiz_discovery.log`). XAM leaves 0 in `RB_HIZCONTROL` and 0x00000FFF in `RB_HIZ_CLEAR`.
- **Bit 4 turns the test to greater-than.** With it, less-than draws over a buffer cleared far were all rejected, and
  greater-than draws over one cleared to 0 drew right and their hidden pixels were rejected; without it greater-than
  draws were tested as less-than ones (`probe_hiz_discovery2.log`). Bits 18 to 26 seem to move the buffer: each hid
  pixels it shouldn't at the end of the target, 512 for bit 18 and twice as many for each bit up; bit 27 hid 479232
  and bit 28 819200, from the top. Bits 2, 3, 5 to 17 and 29 to 31 changed nothing seen. xsdk uses bits 0, 1 and 4.
- **The test ignores the depth compare.** A draw with always and bit 1 was rejected behind a nearer quad
  (`probe_hiz_discovery.log`).
- **A resolve clearing depth with bit 0 sets the buffer from `RB_HIZ_CLEAR`.** Without bit 0 the buffer kept what it
  held: after a near quad at 0.438 and a clear, columns from 0.469 on stayed hidden. With bit 1 too, the buffer
  rejected the copy itself (`probe_hiz_discovery.log`). `RB_HIZ_CLEAR`'s low 11 bits are depth in 2048ths: 0x400 hid
  depth from 0.5 + 1/2048 up and passed 0.50043, 0x200 hid from 0.25, and 0x7FF hid nothing, up to 1.0 with
  less-equal; the bits above are ignored, 0x800 holding 0 and 0xC00 0x400 (`probe_hiz_discovery2.log`,
  `probe_hiz_discovery3.log`). So xsdk writes there the top 11 bits of `RB_DEPTH_CLEAR`'s depth.
- **What changes the buffer.** Killed pixels and draws that test without writing depth left it alone; equal depths
  passed less-equal and equal; 48 overlapping rectangles at random depths, less-than and less-equal, came out right
  over clears to 1 and 0.5 (`probe_hiz_discovery.log`, `probe_hiz_discovery2.log`). The test takes depth bias: a decal
  at 0.51 biased by -0.02 drew over 0.5 everywhere, as with the buffer off, and so did greater-than's mirror of it
  (`probe_hiz_discovery5.log`). It covered a 1280x1024 target whole (`probe_hiz_discovery3.log`).
- **Pixels the test rejects skip stencil.** A far quad failing depth everywhere, its depth-fail operation replacing
  stencil, marked 96 of 921600 pixels with the test on and every one with only the write on
  (`probe_hiz_discovery2.log`).
- **A draw's first block can test old values.** After a clear, or a draw writing the buffer, the next draw tested,
  where its first triangle starts, what the buffer held before: 199 pixels of a full-screen quad, (1248, 0) to
  (1278, 7), its first triangle's part of the top right 32x8 pixels, and 22 at the top right of a 200x200 rectangle;
  in 640x480 and 1280x1024 targets alike, and with the resolve's rectangle square (`probe_hiz_discovery3.log`). After
  a draw nearer, that only rejects less; after a clear or a draw farther, it hides what should show. After each clear:
  `EVENT_WRITE` CACHE_FLUSH changed nothing, and SC_WAIT_WC hung the GPU, after the target was set and in the resolve
  alike (`probe_hiz_discovery4.log`, `probe_hiz_discovery5_wait_in_resolve.log`); one-pixel draws testing the buffer
  left 71 of the 199 pixels; a full-screen draw testing it put every pixel right (`probe_hiz_discovery6a.log`). So did
  the resolve's rectangle drawn once more testing it, in copy mode with the null copy command, in depth-only mode, or
  as two triangles in depth-only mode at z 1 testing less-than or at z 0 testing greater-than, which the buffer
  rejects whole; in no-operation mode that pass hung the GPU (`probe_hiz_discovery7a.log`, `probe_hiz_discovery7f.log`,
  `probe_hiz_discovery5_no_operation_pass.log`).

So xsdk:
- after a resolve that clears depth, writes the buffer from the clear, then draws the resolve's rectangle again in
  depth-only mode, which Xenia takes to skip the pixel shader (xenia:src/xenia/gpu/xenos.h:889-907), with depth
  tested with never and the buffer tested from the side that rejects it whole: at z 0 testing greater-than, or over a
  clear to 0 at the rectangle's z of 1;
- takes the clear's side for the buffer: below 0.5 greater-than, else less-than;
- in `xsdk_gpu_set_depth_stencil`, tests the buffer for compares on that side, never, equal and the clear's two, and
  writes it when they write depth; leaves the test off when a stencil fail operation isn't keep;
- leaves the buffer off until depth is next cleared after a depth write with a compare off that side, which can push
  depth past the buffer's bound, after a target of another size, samples or depth tile, and when a recording begins
  or runs; multisampled targets aren't tried and keep it off.

Then, with the milestone's code on top of milestones 31, 33 and 34 (`a5eaa6f`):
- **probe_hiz** (`probe_hiz.log`): 16 hidden full-screen quads through the long shader took 473 us with the buffer on
  and 40124 us off, 84.8 times as fast, less-than over a clear to 1, and 474 us against 40124, greater-than over a
  clear to 0; every pixel kept the near quad's colour both ways. A resolve clearing colour and depth took 319 us, 290
  clearing colour alone; on `a5eaa6f` they took 290 and 291 (`probe_hiz_main.log`). With the rectangle's second pass
  not rejected, at z 1 over a clear to 1, a resolve clearing depth took 424 us (on `079f061`).
- **xsdk_test passed 282 of 284** (`xsdk_test.log`), the two failures the controller's, which was disconnected
  (`XamInputGetState` returned 0x48F). 8 hidden full-screen quads through the long shader took 239 us, 8 drawn ones
  20065; 12 frames of near and far rectangles, less-than and greater-than over clears to 1, 0.75 and 0, each ending
  with near depth everywhere and the next drawing far over it first, held no wrong pixel. A run before it, the
  controller connected, passed all but one check: pacing at three buffers and every vblank missed one, as runs that
  evening did with and without the milestones (milestone 34, above) (`xsdk_test_pacing_missed.log`). On milestone 32
  alone the same code passed 276 of 276, the controller connected (`xsdk_test_on_32.log`); of two runs before that,
  one missed a vblank in replay (`xsdk_test_on_32_replay_missed.log`), and one's log stopped 42 s in, during scale's
  10-second show, with no exit line (`xsdk_test_on_32_cut_short.log`).
- **Broken on purpose,** the resolve leaving the buffer uncleared when it clears depth, 6 of the 12 frames held wrong
  pixels, 5499024 in all, the far quads hidden where the frame before had drawn near, and that check failed; so did
  raster's, its depth bias cell hidden by the frame before's (`xsdk_test_hiz_uncleared.log`). A first form of the
  check ended each frame by drawing the near quad's colour everywhere, hiding the far quads' rejection: the probe
  timing each frame's first quad took 34 us there, as rejected, and 2512 us with the buffer cleared
  (`probe_hiz_frames.c`, `probe_hiz_frames_break.log`, `probe_hiz_frames_r5.log`).
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14. Against milestone 31's run the scenes' GPU times rose
  4 to 41 us, most by 25 to 30, the second pass of their one resolve a frame clearing depth, and the CPU's times
  stayed within 4%; vulkan4xenon's state scenes write a dword more a draw, `RB_HIZCONTROL`.

With that, milestone 30 is done on the console but for Dusklight's logo shot: draws testing depth skip hidden pixels,
the buffer stays right across depth clears, writes and compare changes, and every title built with it passes.

## Milestones 30 to 35 together, and fence waits that sleep again (2026-10-14)

Milestones 30 to 35 together on main, with vulkan4xenon's and Dusklight's shaders register-packed by xost (`cbf5c3e`),
built clean at `13ee341` (logs: `results/2026-10-14-milestones-30-to-35/`):
- **xsdk_test passed 282 of 284** (`xsdk_test.log`), the two failures the controller's, which was disconnected
  (`XamInputGetState` returned 0x48F). Eight waits after a resolve returned 3.6 to 5.5 us after their fences, and no
  vblank was missed.
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14 (`vulkan4xenon_test.log`, `vkbench.log`); against
  milestone 31's run the GPU's times rose 5 to 41 us, as in milestone 30's.
- **Dusklight's title scene** ran at 29.9, 29.1, 30.0, 29.9 and 29.9 fps (`dusklight.log`), as in milestone 35's run.
- **Dusklight's logo shot**, from the diagnostics build (`dusklight_diag.log`): in phase 0, nothing skipped, the GPU
  took 17.6 ms from submit, where milestone 35's runs at -O2 took 20.1 and 20.3, and 12.2 ms over phases 0 to 8, where
  they took 14.0; the render worker 14.8 ms, from 16.5 and 16.7, waiting less for resolve rectangles. The shot's last
  frame came out the same, byte for byte, as in milestone 29's last run, and so it did in every run below.
- **But its frames took 36.0 ms,** where milestone 35's, without milestone 34's code, took 34.8 and 35.3, the FIFO
  worker 11.5 ms as there.

Aurora runs the game's main thread on hardware thread 0 and its GPU waiter on hardware thread 1, one core
(aurora-xenon:lib/thread.cpp:297-299). The waiter waits on each frame's fence through vulkan4xenon's `vkWaitForFences`
(aurora-xenon:lib/vulkan4xenon/device.cpp:530, vulkan4xenon:src/sync.c:81), so under milestone 34 it looked at the
fence every 5 us for the first 2 ms of each frame's wait. The same diagnostics build, relinked with that look cut to
1 us, and the build as it was ran one after the other:

| Phase 0, nothing skipped | Frame | GPU from submit | FIFO worker | Render worker |
|---|---|---|---|---|
| looking for 2 ms first (`dusklight_diag_spin.log`) | 36.0 ms | 17.6 ms | 11.6 ms | 14.8 ms |
| sleeping between looks (`dusklight_diag_sleep.log`) | 34.3 ms | 17.6 ms | 11.5 ms | 14.8 ms |

Phases 1 to 8 took 34.7 to 36.0 ms a frame with the look and 33.9 to 35.1 without. The game thread waited on Aurora
0.0 ms a frame both ways, so its own work set the frame, and it alone shares the waiter's core: the look took 1.7 ms
of its frame, 5%. So `xsdk_gpu_wait` sleeps between looks again, unless the title calls `xsdk_gpu_set_wait_spin` to
look first for as long as it says (`ee5577d`); xsdk_test's wait check asks for 2 ms.

Built clean with `ee5577d`'s code, every title built against it (logs: the `_waits_sleep` ones):
- **xsdk_test passed 282 of 284** (`xsdk_test_waits_sleep.log`), the two failures the controller's. Its wait check,
  asking for 2 ms, saw each wait return 3.9 to 5.5 us after its fence, and no vblank was missed.
- **Broken on purpose,** `xsdk_gpu_set_wait_spin` ignoring what it's given, those waits returned 43 us to 1.74 ms
  after their fences and the check failed, besides the controller's two (`xsdk_test_spin_ignored.log`).
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14, its times within 1% of the run above; its frames
  look at their fences themselves.
- **Dusklight's title scene** ran at 29.8, 29.1, 30.0, 30.0 and 29.9 fps (`dusklight_waits_sleep.log`), and its logo
  shot at 34.8 ms a frame in phase 0, the GPU taking 17.9 ms, and 33.9 to 34.7 in phases 1 to 8
  (`dusklight_diag_waits_sleep.log`), its last frame as before.

With that, milestones 30 and 31 are done: Dusklight's logo shot draws as before, in less GPU time. A fence wait
returns within microseconds of its fence when the title asks, and otherwise sleeps between looks, leaving the core to
the thread beside it.

## Sockets reach the LAN and the internet when marked insecure (2026-10-14)

`tests/probe_net.c` finds XAM's network exports by ordinal (xenia:src/xenia/kernel/xam/xam_table.inc:15-75) and calls
them as a title, `XNCALLER_TITLE` (xenia:src/xenia/kernel/xam/xam_net.cc:42-47). Every socket is non-blocking, and
every wait has a timeout. Each test runs on a plain socket, then on one given `SOL_SOCKET` option 0x5801, which Xenia
takes as disabling a socket's encryption (xenia:src/xenia/kernel/xsocket.cc:78-82). `tools/net_echo.py` answered on
the PC. Logs: `results/2026-10-14-networking-probe/`, one run with `NetDll_XNetStartup`'s flags at 0
(`probe_net_flags0.log`) and one at 1 (`probe_net_flags1.log`), which came out the same.
- **Starting up.** `NetDll_XNetStartup`, given its 13 bytes with either flags, and `NetDll_WSAStartup` 2.2 both
  returned 0.
- **The link.** `NetDll_XNetGetEthernetLinkStatus` returned 0x0B: active, 100 Mbit/s, full duplex (xam_net.cc:529-535).
- **The address.** `NetDll_XNetGetTitleXnAddr` returned 0x68 at once, DHCP with a gateway and DNS (xam_net.cc:429-450),
  with the console's address, 192.168.0.227, and its MAC.
- **Plain sockets reached nothing.** TCP connections to the PC and to 1.1.1.1:443 never completed, `SO_ERROR` 0, and
  UDP datagrams to the PC never came back. The PC's echo server logged nothing from them (`net_echo.log`).
- **Marked sockets reached everything.** With option 0x5801 set before connecting:
  - TCP to the PC came back byte for byte, and so did UDP;
  - a connection the PC made back to a marked listening socket was accepted, and its line read;
  - TCP to 1.1.1.1:443 connected;
  - an HTTP/1.0 request to example.com on port 80 got `HTTP/1.1 200 OK`.
- **Names resolve.** `NetDll_XNetDnsLookup` found example.com, two addresses, in about 50 ms, with no event given.
  Xenia stubs it (xam_net.cc:542-560).
- **Winsock's values hold.** `NetDll_ioctlsocket` took Winsock's `FIONBIO` (0x8004667E) without an error, and the
  plain sockets' connects returned at once, then waited out their timeouts in `NetDll_select`; the probe doesn't log
  whether they returned `WSAEWOULDBLOCK` or `WSAEINPROGRESS`.

So xsdk's sockets set option 0x5801 on each socket, and the startup flags don't matter.

## Sockets in xsdk (2026-10-14)

xsdk's POSIX sockets (`5de3f6e`, src/socket.c, src/netdb.c) run on XAM's exports, each socket marked with option 0x5801
as it's made. xsdk_test's network checks ran against `tools/net_echo.py` on the PC (logs:
`results/2026-10-14-sockets/`).
- **xsdk_test passed 295 of 295** (`xsdk_test.log`):
  - TCP to the PC and back through `send` and `recv`, and through `write`, `read` and `close`, `fstat` giving
    `S_IFSOCK`;
  - UDP through `sendto` and `recvfrom`;
  - a connection the PC made, accepted;
  - `getaddrinfo` found example.com through `NetDll_XNetDnsLookup`, waiting on an event this time;
  - a blocking connection to a closed port on the console itself got `ECONNREFUSED`;
  - a non-blocking one to the PC completed through `poll`, and `recv` with nothing there gave `EAGAIN`;
  - 1000 sockets opened and closed in turn.

  The whole run took 58 s on the console, with each check now a second at most (`232bdde`).
- **Broken on purpose,** `close` leaving XAM's socket open, the 24th socket of the 1000 failed with `ENOBUFS`, 33
  sockets then open, and that check failed (`xsdk_test_close_left_open.log`); the controller's two failed besides,
  disconnected for that run.
- **vulkan4xenon_test** passed 80 of 80 and **vkbench** 14 of 14, rebuilt against it (`vulkan4xenon_test.log`,
  `vkbench.log`).

What XAM does that POSIX code doesn't expect:
- **No `SO_ERROR`.** `NetDll_getsockopt` refuses it with `WSAENOPROTOOPT`. A build of the checks asking every
  `SOL_SOCKET` option from 0x0001 to 0x0100, 0x1001 to 0x1020 and in five other ranges got answers to 0x0002, 0x0004,
  0x0080 (60), 0x1001 (16384), 0x1002 (17520), 0x1008 (1), 0x5801 (1) and 0x5802 (0) only
  (`xsdk_test_option_scan.log`). None gives a failed connection's cause.
- **A failed non-blocking connection says only that it failed.** `NetDll_select` put it in the write and error sets at
  once. A build of the checks that went on to try the socket (`xsdk_test_after_connect.log`) found:
  - after the refused connection, a second `connect` in progress again, `EINPROGRESS` through xsdk, and `recv` and
    `send` giving `WSAENOTCONN`;
  - after the one that succeeded, a second `connect` giving `WSAEISCONN`, `recv` `WSAEWOULDBLOCK`, and `send` sending.

  So xsdk's `SO_ERROR` is 0 until `NetDll_select` puts the socket in its error set, and `EIO` after.
- **`NetDll_getsockname` gives 0.0.0.0** for a connected socket, with its port; the checks ask the PC what address it
  sees.

## The console keeps only LR's low word across an interrupt (2026-10-14)

Mbed TLS's self-tests, built through xsdk (mbedtls-xenon), passed every suite but SHA-3's: its known answer for a
million 'a's, fed 1000 bytes at a time, came out wrong (`mbedtls_selftest.log`). Logs, and the probes' sources:
`results/2026-10-14-lr-high-word/`.
- **Wrong only sometimes.** `sha3_probe2.c` hashed 2000 and 1,000,000 'a's in 1000-byte updates with Mbed TLS's
  `sha3.c` built several ways. 2000 always came out right. The million came out right at -O0, and wrong at -O1, -O2
  and -O2 with inlining off (`sha3_o0.log` and the rest), and the same -O2 build gave a different wrong digest each
  run (`sha3_o2.log`, `sha3_o2_again1.log`, `sha3_o2_again2.log`).
- **The permutation against itself.** Mbed TLS's Keccak-f[1600], built at -O2 and at -O0, run on the same states:
  - 65 of 100,000 differed, and each -O2 result, recomputed ten times from its input, came out as -O0's ten times
    (`keccak_diff_first.log`);
  - on every hardware thread, 4 to 52 of 50,000 (`keccak_diff_threads.log`);
  - whatever the stack below held, zeros, ones or 0x5A (`keccak_diff_stack_filled.log`);
  - without update forms (`-mno-update`), without `rotld`, and with theta unrolled, which drops `mulhwu`
    (`keccak_diff_variants.log`, `keccak_diff_theta.log`).
- **On the clock.** The -O2 permutation, run on one input for 2 s and checked against -O0's answer, failed 126 times
  in 203,712, every lane wrong, and every gap between failures was a whole number of milliseconds of the 49.875 MHz
  time base: 995, 1994, 2994, 3989 us and so on (`keccak_diff_timing.log`, `keccak_diff_timing.c`). So the 1 kHz
  clock interrupt broke it whenever it landed in the wrong place.
- **Not the GPRs, the stack or the arithmetic.** Through half-second spins of the time base:
  - 64-bit patterns in r6-r12 and r14-r31 came back whole (`regs_probe.log`);
  - so did r0, r3-r5, r10-r12, CTR, CR1-CR7 and XER (`regs_probe2.log`);
  - so did a 512-byte frame filled with patterns (`mem_probe.log`).

  The same 64-bit integer work done twice never differed: 60,000 times in registers and 15,000 through memory, on
  hardware threads 0, 2 and 4 (`alu_probe.log`).
- **LR.** Under register pressure GCC's -O2 permutation parked 64-bit lanes in LR, `mtlr` and later `mflr`. Held in
  LR through half a second, A1B2C3D4E5F60718 came back as 00000000E5F60718, every round, while CTR held it whole
  (`lr_probe.log`).

So xsdk's GCC keeps LR to 4-byte modes under -mcall-xsdk (tools/toolchain/gcc-xsdk.patch,
`rs6000_hard_regno_mode_ok_uncached`, xsdk `158c339`), and spills such values to the stack. LR still holds
addresses and other 32-bit values, such as a table pointer or a loop's end, whose low word survives. xsdk_test's
`interrupts` check runs a Keccak-f[1600] with every lane a variable through half a second of clock interrupts,
checking each result against the all-zero state's known permutation:
- **Built with the toolchain before it** (`~/xsdk-gcc-lr64`), 163 of 259,065 came out wrong and the check failed
  (`xsdk_test_old_toolchain.log`); its permutation had 13 `mtlr` and `mflr`.
- **Rebuilt in full with the fix,** every title clean:
  - xsdk_test passed 294 of 296, the two failures the controller's, disconnected: 266,343 permutations, none wrong
    (`xsdk_test.log`);
  - vulkan4xenon_test passed 80 of 80 and vkbench 14 of 14, its CPU times within 1.6% of the run before and its GPU
    times within 10 us (`vulkan4xenon_test.log`, `vkbench.log`);
  - Dusklight's title scene ran at 29.1 to 30.0 fps with 123 MB free, as before (`dusklight.log`), and its logo shot
    at 34.8 ms a frame, the GPU taking 17.6 ms, its last frame the same byte for byte (`dusklight_diag.log`);
  - Mbed TLS's self-tests passed, all 30 suites (results/2026-10-14-tls/).

## TLS 1.2 through Mbed TLS (2026-10-14)

mbedtls-xenon (`e59cc68de` to `1500b8ac2`) builds Mbed TLS 3.6.7 for the console on xsdk's sockets, `getentropy`
and clocks, TLS 1.2 only (docs/scope/networking.md, step 3). Built with the toolchain above, its two check titles ran
on the console (logs: `results/2026-10-14-tls/`):
- **The self-tests** passed, all 30 suites, SHA-3's long known answers included (`mbedtls_selftest.log`).
- **TLS 1.2** (`mbedtls_https.log`), with Mozilla's root certificates from `cacert.pem` beside the title, all 121
  parsed, and the console's clock at 2026-10-14 08:34:34 UTC:
  - example.com shook hands in 133 ms with ECDHE-ECDSA and ChaCha20-Poly1305, its certificate checked against its
    name, and its front page answered `HTTP/1.1 200 OK`;
  - badssl.com's own certificate was accepted, in 319 ms with ECDHE-RSA and AES-128-GCM;
  - wrong.host.badssl.com, expired.badssl.com and self-signed.badssl.com were refused, each certificate for its own
    reason: another name, expired, not signed by a trusted root.
- **Broken on purpose,** a verify callback clearing the name mismatch, wrong.host.badssl.com was accepted and that
  check failed, 6 of 7 passing (`mbedtls_https_name_check_left_out.log`).
- **The random values differ between runs.** example.com's client random, its first four bytes the time, held
  `CDDF4AEE0E4B0A64...` in the first run and `7C93B7E9EBB098EF...` in the second.

## The texel rate, milestone 36 (2026-10-14)

A pixel shader taking four bilinear samples a texel apart, its coordinates worked out from a constant, sampled about
10 texels a clock through xost, of the texture units' published 16 (docs/scope/36-texel-rate.md).
`tests/probe_texel_rate.c` times hand-built layouts of that shader, written in xost's assembly
(tools/gen_texel_shaders.py), and xost's own build of its GLSL. Each fills a 1280x720 target with 4 samples a pixel
from a 256x256 ramp texture, checked after against the four texels' average, timed as milestone 31's probe times its
fills ("The pixel rate, milestone 31"); a clock is the published 500 MHz. Logs: `results/2026-10-14-milestone-36/`.
- **The fetches together reach the 16; between their coordinates' work, they don't.** Bilinear, with each layout's own
  registers under xsdk's split of 64 pixel registers (`probe_texel_rate.log`; point ran within 0.06 of each):

  | Layout | Registers | Texels a clock |
  |---|---|---|
  | Reference: four fetches at the interpolated coordinates | 5 | 15.92 |
  | xost's before: each add, then its fetch, the coordinates through one register | 6 | 10.17 |
  | The same order, each coordinate in a register of its own | 8 | 8.82 |
  | The coordinates first, then the four fetches together in an exec of their own | 8 | 15.90 |
  | Two coordinates to a register, then the fetches together | 7 | 15.92 |
  | xost's build now | 8 | 15.90 |

  So the layout held the rate, not the registers: a register for each coordinate, its fetch still after its add, ran
  slower than sharing one.
- **The wait bit changed nothing.** Each layout with it on the first instruction reading a sample
  (xenia:src/xenia/gpu/ucode.h:230-231) ran within 0.01 a clock of the same layout without.
- **More registers held fewer threads, as milestone 31 found.** Bilinear, with 12 and 17 registers declared:

  | Layout | 12, split 64 | 17, split 64 | 12, split 112 | 17, split 112 |
  |---|---|---|---|---|
  | Reference | 14.09 | 9.19 | 15.99 | 15.32 |
  | xost's before | 6.09 | 3.88 | 10.01 | 7.08 |
  | Each coordinate in its own register | 6.10 | 3.88 | 10.06 | 7.16 |
  | The fetches together | 12.24 | 7.53 | 17.01 | 15.04 |
  | Two coordinates to a register | 12.56 | 7.73 | 16.50 | 14.99 |
  | xost's build now | 12.24 | 7.53 | 16.61 | 14.97 |

  With their own registers under 112 pixel registers the reference ran 16.76, the fetches together 15.92 and xost's
  layout from before 12.29. Rates past the published 16 came only under that split, timed as the others are; what
  allows them isn't looked into.
- **A 64-bit texture, trilinear,** R16G16B16A16F, 512x512 with every level, drawn 1.41 texels a pixel so levels 0
  and 1 blend: the fetches together ran 3.93 a clock with their own registers and with 12, the reference 3.96, xost's
  layout from before 1.21 and 2.10, and each coordinate in its own register 1.24 and 2.10.
- **xost lays a pixel shader's fetches out together now** (xost `fe34a9d`, `_group_fetches`): the work their
  coordinates need first, then the fetches, a run of two or more in an exec of its own, what they read kept till the
  run ends. Its build of the probe's GLSL is the "together" layout but for its sum, and the probe judged it
  `xost's build 15.90 a clock, the fastest hand-built (two coordinates to a register, the fetches together) 15.92:
  within 5%`.
- **Broken on purpose,** xost's build laid out as before (`gen_texel_shaders.py --before`), it ran 10.17 a clock and
  the probe logged `FAIL, more than 5% slower` (`probe_texel_rate_xost_as_before.log`). The first such run went
  through every measurement, but the console stopped answering on the network before the verdict reached its log; it
  answered again only once switched off and on, and the run again went through, verdict and all. What stopped it
  isn't known.
- **Dusklight's shader pack**, rebuilt with it, holds all 145 programs within xsdk's split. 28 of its 98 pixel
  programs are laid out anew: the longest run of fetches grew in 24 of them, 13 from 1 to 2 and 6 to 4, and one
  grew from 12 registers to 14. Each gave the same exports as before on xost's simulator, from the same random
  interpolators, constants and texture, three times (`compare_pack.txt`); with the layout broken on purpose to ignore
  what each instruction reads and writes, 5 differed (`compare_pack_dependencies_ignored.txt`). vulkan4xenon's shaders
  all sample at their interpolated coordinates and came out byte for byte the same.
- **Dusklight with that pack** (`dusklight.log`) ran its title scene at 29.0 to 30.0 fps in 10-second windows once
  loaded, as the last run did (29.1 to 30.0, "The console keeps only LR's low word across an interrupt"), its
  capture the same scene at another point of the camera's pan. Its logo shot (`dusklight_diag.log`, the diagnostics
  build) took 17.6 ms of GPU time a frame, as in that run, and 35.0 ms a frame against 34.8; the end frame is the same
  pixel for pixel, and the start frame differs in 0.05% of its pixels, the logo's shine, as earlier runs' start
  frames differ from each other (0.05 to 0.07%). So the layout didn't change what the shot's GPU time goes on; which
  of the programs laid out anew the shot draws with isn't looked into here.
- **xsdk_test passed 294 of 296** (`xsdk_test.log`), the two failures the controller's, none connected;
  **vulkan4xenon_test** 80 of 80 and **vkbench** 14 of 14 (`vulkan4xenon_test.log`, `vkbench.log`), vkbench's CPU
  and GPU times within 1.3% of the last run's.

With that, milestone 36 is done: xost lays a pixel shader's fetches out together, and four samples at worked-out
coordinates run 15.9 texels a clock where they ran 10.2, and 3.9 from a 64-bit texture filtered trilinearly where
they ran 1.2. Left for later, as agreed: a register split giving pixel shaders more, under which 12 registers ran
16.6 a clock rather than 12.2.

## glslang on the console (2026-10-14)

glslang 16.3.0 builds for xsdk with the toolchain, as a static library a title links (glslang-xenon `1d9d8b04`), and
compiles GLSL to SPIR-V on the console (docs/scope/shader-compiling.md, step 1). Its probe (glslang-xenon
`8755a431`, `xsdk/glslang_probe.cpp`) compiles each of the 145 GLSL programs in Dusklight's shader pack as
`glslang -V` does on the PC: Vulkan 1.0 rules, SPIR-V 1.0, parsed, linked alone and turned into SPIR-V, on a thread of
its own with a 1 MB stack. It compares each program's SPIR-V with what the pack holds, which the PC's glslang 16.3.0
made, by its length and its FNV-1a hash as vulkan4xenon takes it. Logs: `results/2026-10-14-glslang/`.
- **All 145 came out the same as on the PC** (`glslang_probe.log`).
- **The first took 0.94 s,** as it also has glslang set up its built-in symbols; the first vertex shader after it took
  8 ms, so that covers both stages. The other 144 took 11.3 ms each on average, 1.63 s together, and 30 ms at most,
  `407248b726bcb428.vert`. On the PC, the same glslang took 15.1 s for the 145, each run in a process of its own.
- **Heap:** 13.5 MB in use at most after a program, glslang's built-in symbols among it, and the heap grew to 30.5 MB
  at most. Dusklight's title scene runs with 118 MB free (vulkan4xenon:docs/ROADMAP.md, milestone 12).
- **Stack:** 68 KB at most below the compiling thread's first frame, from the stack painted before the first program
  and read after the last.
- **Broken on purpose,** built to target SPIR-V 1.3, every program compiled and none came out the same, and that check
  failed (`glslang_probe_spirv_1_3.log`).

## Containers name xost as their creator (2026-10-14)

xost wrote the XDK compiler's version, 2.0.11626.0, as the creator in the constant table of every container it made,
as the containers it was first checked against had it; xsdk's own generators already wrote `xsdk`
(tools/gen_triangle_shaders.py). xost writes `xost` now, in Python and in xcomb, and
`xenos-toolchain/tests/test_container_creator.py` checks each way it makes a container. libxenon's loader reads the
header's magic and the shader record, not the table's strings (libxenon:drivers/xenos/xe.c:614-629), and xsdk's reads
the header, the microcode and its record (src/shader.c). Logs: `results/2026-10-14-xost-creator/`.
- **174 containers carried the XDK's version:** the texel rate probe's 10, vulkan4xenon's 19 and the 145 in
  Dusklight's pack. Rebuilt, each differs from before only in its creator: the same microcode, program control,
  context and shader record (`compare.py`), and the SPIR-V beside them is unchanged.
- **On the console** every title built with them ran as before: xsdk_test passed 294 of 296, the controller's two;
  vulkan4xenon_test 80 of 80; vkbench 14 of 14, its times within 0.2% of milestone 36's run; the texel rate probe as
  then, xost's build 15.90 a clock; Dusklight's title scene at 29.1 to 30.0 fps once loaded, and its logo shot in
  17.7 ms of GPU time a frame against 17.6, the end frame the same pixel for pixel.
- **Broken on purpose,** xost's default creator put back, the creator check failed for every way xost makes a
  container; xcomb's alone put back, xcomb's parity with the Python combiner failed.

## xost's compiler in C on the console (2026-10-14)

xost's compiler, SPIR-V to a Xenos container, is C in libxsdk now (xsdk `02dede7`, src/xsc_*.c), so a title can
compile shaders on the console (docs/scope/shader-compiling.md, step 2). Its probe (`tests/probe_xsc.c`, its programs
from Dusklight's pack through `tools/gen_xsc_probe.py`) compiles the SPIR-V of each of the pack's 145 programs at
xsdk's register split with the optimiser on, as the pack was built, on a thread of its own with a 1 MB stack, and
compares the container, literals and resources with the pack's, which the Python compiler made on the PC. It also
compiles two shaders xost refuses. Logs: `results/2026-10-14-xsc-c/`.
- **All 145 came out as the pack's,** and both refusals gave the PC's messages (`probe_xsc.log`).
- **1.33 s for the 145:** 9.1 ms a program on average, 76 ms at most, `407248b726bcb428.vert`, the program glslang
  took longest over too. With glslang's 11.3 ms ("glslang on the console"), GLSL becomes a container in about 20 ms.
- **Heap:** the heap grew to 1.06 MB at most, from 32 bytes in use; what stayed in use between programs never passed
  5.7 KB, as each compile frees its memory at its end.
- **Stack:** 30 KB at most (29,840 bytes), from the stack painted before the first program and read after the last.
- **Broken on purpose,** built with the fetch layout off, 117 of the 145 matched and the probe failed
  (`probe_xsc_break.log`).
- **On the PC,** the C gave the same bytes as the Python for all 1068 compilations of xost's testdata, vulkan4xenon's
  shaders and Dusklight's pack over both register splits, the optimiser off and on (`parity.txt`, run at `02dede7`
  while the Python was there); its encoder matched on 8000 random instructions (`encode.txt`). Rebuilt through it,
  vulkan4xenon's shader files and Dusklight's pack are byte-identical to those committed (`rebuild.txt`).
- **xsdk_test checks it on every run:** two of xost's test shaders, a vertex shader with blocks and a literal and a
  pixel shader with four fetches laid out, compile into the container, literals and resources the PC makes
  (`tests/test_xsc.c`). Both passed, 296 checks in all and the controller's two failing as ever (`xsdk_test.log`);
  with the fetch layout off, the pixel shader's failed (`xsdk_test_break.log`).

## Files renamed on the console (2026-10-14)

`rename` opens the file for `DELETE` and hands `NtSetInformationFile` the rename class, 10, with 16 bytes
(xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:170-172): whether to replace what's at the new name, a byte padded
to 4; a root directory handle, 0; and the new name as an ANSI string, the whole kernel path, `game:\...`, as xsdk opens
files by (src/path.c). `std::filesystem::rename` comes to it too. Logs: `results/2026-10-14-rename/`.
- **A file renamed over another takes its place,** and its old name is gone; a folder with a file in it renames, the
  file going with it (xsdk_test, `xsdk_test.log`).
- **The first byte is the replace flag:** with it 0, renaming over a file fails with 0xC0000035,
  `STATUS_OBJECT_NAME_COLLISION`, and both checks failed (`xsdk_test_break.log`); the folder still renamed.
- **Xenia renames nothing:** its handler takes the class and has no case for it (xboxkrnl_io_info.cc:202-251).
- **A folder within a folder doesn't go through `remove_all`,** as xsdk opens everything sharing nothing and libstdc++
  deletes the inner folder while it still holds it open: 0xC0000043, a sharing violation (TODO.md).

## Shaders compiled as the game asks (2026-10-14)

Step 3 of docs/scope/shader-compiling.md: a program Dusklight's pack doesn't hold is compiled on the console the first
time the game asks for it, its GLSL to SPIR-V through glslang-xenon in Aurora's vulkan4xenon backend and the SPIR-V
to a container through xost's compiler in vulkan4xenon, and both are kept on the hard disk, each file written beside
its name and renamed into place ("Files renamed on the console"). vulkan4xenon's folder is named after
`xsdk_xsc_version()` (xsdk `9f12704`), Aurora's after glslang's version. vulkan4xenon `a624f2e` and `da2ea21`, Aurora
`953d207` and `2ba57e8`, Dusklight `828f1f561d` and `1d0abc8611`. Logs: `results/2026-10-14-shader-compiling-step3/`.
- **Without the pack, nothing kept** (`dusklight_nopack_1.log`, built with `DUSK_XSDK_GLSL_PACK=OFF`): the boot
  screens and a minute of the title scene asked for 107 programs, all compiled. Through glslang the first took 919 ms,
  glslang starting with it, and the other 106 24.4 ms each on average, 61 ms at most; through xost 20.5 ms on average,
  126 ms at most. The title scene ran at 28.7 to 30.0 fps, drawn as with the pack (`title_nopack_1.png`,
  `title_pack.png`, at another moment of the camera's pan).
- **The next runs read them back:** the second read all 107 and compiled the 3 programs only it reached, at its exit;
  the third read those 110 and compiled 1 more (`dusklight_nopack_2.log`, `dusklight_nopack_3.log`). glslang started
  only for those. The second run held 50 s longer on the boot's warning screen, nothing compiled or read meanwhile, as
  the run before step 3 had (`results/2026-10-14-xost-creator/dusklight.log`).
- **What the console compiled is the PC's:** the 111 programs' SPIR-V is the pack's word for word, and their 111
  containers its bundle's byte for byte, with as many literals and resources (`compare_cache.txt`).
- **Played by the user** from nothing kept: the title, a new file, the opening cutscenes and Ordon, stages F_SP102,
  F_SP108, F_SP103 and F_SP104 (`dusklight_played_1.log`). 189 programs compiled, none refused: through glslang the
  first in 919 ms and the rest in 23.9 ms on average, 64 ms at most; through xost 17.0 ms on average, 136 ms at most.
  The second time through the same stages compiled nothing, and read 159 programs and their 159 containers back
  (`dusklight_played_2.log`).
- **The memory card** wasn't made the first time: Aurora's atomic write named its temporary file after the card's,
  44 characters where FATX holds 42, and each create failed with 0xC0000033, `STATUS_OBJECT_NAME_INVALID`. Named in 21
  characters (Aurora `2ba57e8`; its io tests, `aurora_io_tests.txt`, and with the old name put back
  `aurora_io_tests_old_name.txt`), the second run made it, `01-GZ2E-gczelda2.gci`.
- **Ordon's minimap drew as noise** (`minimap_noise.webp`, the user's capture): the vulkan4xenon backend doesn't convert
  EFB copies through a palette, so the texture the map is drawn from is never written. In the first played run free
  memory also reached 0, the GPU's growing from 53 MB to 143 MB, and 204 of those textures weren't made; the second,
  glslang never started, kept 65 MB free or more. Both are proposed as vulkan4xenon's milestones 14 and 15 (its
  docs/ROADMAP.md).
- **With the pack** (`dusklight_pack.log`; the diagnostics build, `dusklight_diag.log`) nothing was compiled or read.
  The title scene ran at 28.6 to 30.0 fps, and the logo shot took 17.6 ms of GPU time a frame against 17.7 in the run
  before, its end frame the same byte for byte.
- **xsdk_test** passed 300 of 300, the controller's two among them; **vulkan4xenon_test** 94 of 94; **vkbench** 14 of
  14, its draw and submit times within 2.5% of the run before.

## Serialized fetches (2026-10-15)

Within an exec, a pixel shader's ALU instructions and its fetches run apart on the console, and an instruction waits
for the other's results only when its serialize bit is set, the second of its two bits in the exec's sequence
(xenia:src/xenia/gpu/ucode.h:231). xost set none. Aurora's palette conversion for Ordon's minimap (vulkan4xenon's
milestone 14) fetches an index, works out the palette's coordinate from it and fetches there; laid out
(`palette_unserialized.txt`), the ALU instruction reading the index came right after its fetch, in the same exec. The
console drew noise, and Xenia, which runs each instruction to its end in turn, drew the map, from the same containers:
the 41 shaders the console compiled were Xenia's byte for byte. Logs and dumps: `results/2026-10-15-serialized-fetches/`,
the variants in `variants.txt`, each the conversion's target dumped from memory.
- **Before** (`variants_a_unserialized.png`): the palette at the interpolated uv and the index as grey came out right;
  the conversion, and a fetch from the copy at coordinates worked out from its own index, came out as noise.
- **Fetches serialized** where they read a temp the ALU wrote in the exec (`variants_a_fetches_serialized.png`,
  `variants_b_fetches_serialized.png`): no change. The fragment's push constants read right (set b's V0, grey), and so
  did a fetch at coordinates the ALU worked out from them (V2); the conversion with its scale as literals didn't (V1).
- **So the ALU read the index too soon.** In each program that drew right, an ALU instruction read a fetch's result in
  a later exec, as the grey variant's does and as a vertex shader reads its vertex fetches. That the exec boundary was
  the wait was wrong: a slow fetch's result read in the next exec came too soon as well ("Slow fetches across execs",
  2026-10-17).
- **xost then** (src/xsc_program.c, exec_entry) serialized an ALU instruction that read or wrote a temp a fetch in its
  exec wrote since the exec's last serialized instruction, and a fetch that read a temp the ALU wrote there
  (`palette_serialized.txt`); it now does so across execs ("Slow fetches across execs"). All four of set b drew right
  (`variants_b_serialized.png`), the conversion among them, and Ordon's minimap draws as on the PC (vulkan4xenon's
  `results/2026-10-15-milestone-14/`). The fetch's half is a precaution, not tried without: V2's fetch was serialized
  and drew right.
- **What it changes:** 2 of the 145 programs in Dusklight's pack (`820f98f280587ea8.frag`, `ca44599827bbdb23.frag`),
  besides the conversion's two. The logo shot took 17.6 ms of GPU time a frame as before, its end frame the same byte
  for byte (`dusklight_logo.log`). xsdk_test passed 300 of 300, vulkan4xenon_test 100 of 100 and vkbench 14 of 14, its
  draw and submit times within 0.6% of the run before.
- **Broken on purpose,** the bit never set, xost's test (`tests/test_dependent_fetch.py`) fails, and on the console the
  conversion drew noise, as set a did.

## Depth resolves (2026-10-15)

`xsdk_gpu_resolve_depth_texture` copies eDRAM's depth and stencil into a tiled `XSDK_FORMAT_D24S8` texture: a resolve
with `RB_COPY_CONTROL`'s source 4, depth (xenia:src/xenia/gpu/registers.h:856), into `k_8_8_8_8` swapped 8-in-32, as
Direct3D 9 copies depth (xenia:src/xenia/gpu/draw_util.cc:968-972). A depth resolve can't clear colour
(xenia:src/xenia/gpu/xenos.h:925-931), so its colour clear is masked out. xsdk_test's depth check draws the overlaps
again and resolves their depth with no clears (tests/test_depth.c, check_depth_texture). On the console
(`results/2026-10-15-depth-resolves/`):
- **Each word the CPU reads** is the sample's depth in its top 24 bits above stencil in its low 8, exactly:
  `60000000` for depth 0.375 (`600000`), `40000000` for 0.25, `80000001` for 0.5 inside the stencil mask.
- **Sampled as `k_24_8`,** format 22 with the swizzle depth, 0, 0, 1, a fetch returns the depth from 0 to 1 in red:
  written to an 8-bit target, 0.375 came out 96, 0.25 64 and 0.5 128, as rounding gives. Xenia gives 127 for 0.5.
- **Broken on purpose,** the resolve copying colour target 0, the words hold the overlaps' colours (`FF4040E0`), and
  both checks fail.
- vulkan4xenon resolves a sampled depth attachment with it when a render pass leaves the attachment to be read, and
  Aurora answers GXPeekZ and makes Twilight Princess's depth copies from it (vulkan4xenon's milestone 14, step 5).

**The replay and pacing checks read their stats too late.** "replay: no vblank missed" failed in 2 of 3 console runs
with the depth checks in and passed in the run from HEAD before them. Each miss came after the last frame: a test
waited for the GPU, checked the frame and wrote the log before closing the present, and a vblank in that time found no
frame queued and counted as missed ("30 flips over 31 vblanks (first 1, last 30), 1 missed";
`xsdk_test_replay_missed.log`, `xsdk_test_replay_missed_earlier.log`, `xsdk_test_head.log`). Both tests now read the
present's stats as soon as the last frame has flipped (tests/test_pacing.c, test_present_stats_at_last_flip). Twice
since, xsdk_test passed but for the controller's two checks, with no pad attached (`xsdk_test.log`, `xsdk_test_2.log`),
and with the pad, 305 of 305 (`xsdk_test_pad.log`).
Broken on purpose, the replay test sleeping 40 ms halfway, "no vblank missed" and "a flip every swap interval" fail
(`xsdk_test_break_replay.log`).

## The Guide shows through VdSwap (2026-10-15)

The Guide never opened in a title built on xsdk: the button sounded, and the Guide showed only once the title had
exited. A probe drew frames through `xsdk_present` and called `VdSwap` on each, as D3D does (xenia:src/xenia/kernel/
xboxkrnl/xboxkrnl_video.cc:352-360), while the user opened and closed the Guide (logs: `results/2026-10-15-guide/`,
`probe_1.log` and `probe_2.log`; the probe isn't kept, as `xsdk_present_end` now does the same).

- **XAM tells the title** with notification 9, `XN_SYS_UI` (xenia:src/xenia/kernel/xam/xam_ui.cc:72-75), from a
  listener `XamNotifyCreateListener` made: parameter 1 as the Guide opened, 0 as it closed.
- **While the Guide is open, `VdSwap` writes more than its fence.** First the frame's front buffer into texture fetch
  constant 15 (register 0x485A), its address made physical, clamping and linear filtering added; then
  `PM4_INDIRECT_BUFFER` to XAM's commands, from three buffers in turn (0x01DB0000, 0x01DF0000, 0x01E30000) of 0xA2A to
  0x2720 dwords; then the fence. Closed, the fence alone (`probe_1.log`, frames 0 and 59).
- **`VdGetSystemCommandBuffer`'s 0x94 bytes say so too:** XAM's commands' address and length; a surface of XAM's
  (0xE1C18000), format 6, colour space 0, 852x480; on some frames, with word 7 set, words 8 to 0x15 giving 0, 0,
  852x480 and 1920x1080; and with word 0x24 set, words 0x16 to 0x23 giving 0, 0, 640x480 and 1920x1080. Closed, zeros.
- **XAM's commands draw into its own surface,** not the title's: a scissor 852 wide (`PA_SC_WINDOW_SCISSOR_BR`
  0x03D40354), a target 880 wide (`RB_SURFACE_INFO` 0x0D800370), and a resolve to 0x01C19000, 864 wide and 480 high,
  `RB_COPY_DEST_INFO` 0x01000300: `k_8_8_8_8` swapped and not byte-swapped, as `xsdk_gpu_resolve` writes front buffers.
  They write no display register (`probe_2.log`, each buffer walked). Their type 3 packets are draws, shader loads,
  constant loads, invalidates, waits and events: no `ME_INIT`.
- **The first opening held a frame up 2.7 s,** and the next few took 120 to 290 ms each; later openings didn't slow the
  frames.
- **Nothing showed:** the title flipped its own buffers.
- **Reading XAM's buffers from the title killed it,** twice, while the Guide first opened: the screen froze on the
  title's last frame and the console kept answering over the network, as the title had ended without giving the GPU back
  (`probe_3_killed.log`, `probe_4_killed_live.log`, both ending inside a read of XAM's buffer through the 0xC0000000
  window). The probe then read none of XAM's memory (`probe_5.log`, in which the Guide wasn't opened: the controller
  had dropped off with a reboot, as `XamInputGetState`'s 0x48F says in xsdk_test).

**What the title is to do, from the console's kernel and XAM.** Copied page by page from a title on the console
(kernel 2.0.17559.0, base 1888) and disassembled, a local step kept out of this repository:

- **The kernel's `VdSwap`** (0x800C8B90) writes the fetch constant and the indirect buffer when the system command
  buffer's word 0 is set, the address made physical as the low 29 bits plus 0x1000 for the 0xE0000000 window; and when
  word 2 is set it writes words 2 to 6 through the pointers it was given for the front buffer's address, format, colour
  space, width and height (0x800C8C70 to 0x800C8C98). It ends by calling `VdCallGraphicsNotificationRoutines(8, 0)`.
- **The kernel keeps three system command buffers** that XAM sets in turn with `VdSetSystemCommandBuffer`
  (0x800C8AB8). `VdGetSystemCommandBuffer` (0x800C8B48) copies the newest, and gives a value counting up 2 a call,
  kept with it; `VdQuerySystemCommandBuffer` (0x800C8A08) tells XAM whether the GPU has written a buffer's value at the
  address `VdSetSystemCommandBufferGpuIdentifierAddress` gave (0x800C8940), that is, whether it's done with it.
- **XAM's routine for the graphics notifications** (0x816CBFD8, set through `VdRegisterXamGraphicsNotification`)
  times every swap at event 8. At event 1 it takes 64 bytes describing the title's display; their last 56 come back
  in words 0x16 to 0x23 (0x816CD674).
- **D3D sends event 1** (XAM's copy, 0x8184F578 and 0x8184F3D8): the front buffer's width and height and the output's,
  four halfwords; then the scaler between them, the source's left, top, right and bottom (0, 0 and the front
  buffer's size when all four are 0), the output's width and height (the display's when 0), and eight words of filter
  settings, 0 for the defaults. The kernel's own calls are other events: `VdInitializeEngines` sends 4 and
  `VdShutdownEngines` 5, with 12 bytes of the display's (0x800C7D68). xsdk sent no event 1, so the 640x480 was what
  XAM held from before the title took the GPU.
- **XAM's present** (0x81841AE4 to 0x81841E68), D3D's own: after `VdSwap`, when word 2 was set and the colour space
  matches its own, it shows the surface `VdSwap` gave back, at the size it gave back, with the scaler of words 8 to
  0x15, kept from the last frame word 7 was set; when word 2 is 0 again, its own buffer and scaler.

So a title shows the Guide by swapping every frame and showing XAM's surface while `VdSwap` hands it back: milestone
37 (docs/scope/37-guide.md).

## The Guide, milestone 37 (2026-10-15)

`xsdk_present_end` hands every frame to `VdSwap` in the ring itself, shows XAM's surface while `VdSwap` gives it back,
and `xsdk_present_open` tells XAM the title's display through event 1 (docs/scope/37-guide.md). On the console
(`results/2026-10-15-guide/`, and vulkan4xenon:results/2026-10-15-guide/):

- **xsdk_test passed 313 of 313** (`xsdk_test.log`). In each pacing mode all 30 frames went through `VdSwap` and its
  last fence landed (`30 swaps, fence 000265E7, landed 000265E7`), and with the Guide closed XAM drew none. With the
  swap broken on purpose, NOPs in its room, those four checks failed and no other (`xsdk_test_break_swap.log`, 309 of
  313). An earlier run, after a reboot had dropped the controller, failed only the controller's two checks
  (`xsdk_test_no_pad.log`).
- **guide_check passed 8 of 8** (`guide_check.log`): the user opened the Guide three times; XAM drew 269 frames, and its
  surface showed in place of each; at every close the title's own frames came back, every frame held its bar, and 1
  of 67,528 frames missed its vblank, the longest wait for a front buffer 20 ms. The user saw the Guide over the whole
  frame, the bar moving under it. An earlier run failed the check on the title's frames after the close only because
  it wanted a second of them, and the user pressed Back 0.6 s after closing (`guide_check_quick_back.log`); the check
  now asks that no frame after the close showed XAM's surface and that the title's own came after it.
- **Broken on purpose,** present never showing XAM's surface: the Guide stayed hidden, XAM drawing 3,551 frames and
  none showing (`guide_check_break_show.log`). That build also left the set-up unqueued after XAM's commands, and
  every frame still held its bar, so no check shows the set-up is needed; it stays, as XAM's commands set registers
  xsdk sets only at open.
- **Without event 1,** the frame under the Guide was its top left 640x480, stretched over the screen. Dusklight showed
  it first: the HUD's "ITEMS" sat at about (185, 450) under the Guide and (90, 300) in the game, 2 times across and
  1.5 down (`dusklight_zoomed.log`, the user's captures). Broken on purpose so present sends no event 1, guide_check's
  bar under the Guide was zoomed the same way, as the user saw (`guide_check_break_describe.log`); no check catches
  it, as the title can't read XAM's surface.
- **Dusklight** with event 1 (`dusklight.log`): the user opened the Guide in game and saw it over the whole frame,
  Link and the HUD where they belong; the game held 30 fps; leaving through the Guide's Xbox Home took the console back
  to Aurora, with no reboot.
- **vulkan4xenon_test passed 112 of 112 and vkbench 14 of 14,** this morning's vkbench and the new one twice each in
  turn (vulkan4xenon:results/2026-10-15-guide/). Each build repeated its times to within 2 us. The new one's draw
  scenes record 0.7 to 2.7% slower, vulkan4xenon's `draws_1000` 1,241 us against 1,263 and `draws_4000` 5,193 against
  5,286, xsdk's `states_1000` 977 against 1,003, and vulkan4xenon's uniform scene 3.4% faster, 740 against 715. The
  differences grow with the draws, `draws_4000`'s four times `draws_1000`'s, where the swap comes once a frame and
  outside the times; the code the draws run didn't change, only where it lies.
- **Ending with the Guide open hangs.** The break run's title ended on its own with the Guide open and never finished
  ending: it logged its exit, and the console was rebooted 15 s later. XAM's Guide thread, stopping,
  spins until the GPU has written the fence of the system command buffer it last set, then sets an empty one and spins
  again (xam 0x816C9A20, called at 0x816CE3AC), and no swap after the title's last can write it. Leaving through Xbox
  Home ends the title while it's still swapping, and went cleanly. guide_check and Dusklight end only through the
  controller or Xbox Home, and XAM keeps the controller from them while the Guide shows; a title that ends by itself,
  as xsdk_test does, would hang if the Guide were open then.

## The cost of a draw (2026-10-15)

`build/probe_draw_cost.xex` (`tests/probe_draw_cost.c`) times vkbench's cube a call at a time, for milestone 38 (logs:
`results/2026-10-15-milestone-38/`): the matrix, 4 vertex constants; the tint, 1 pixel constant; the indexed draw.
Each variant runs 4,000 cubes a round for 9 rounds, and the best round is reported in CPU clocks a cube (the time base
ticks every 64). Variants are recorded into a command buffer the GPU never runs, so only the CPU's work counts and any
layout is safe to time; those marked "through the stream" run with the GPU open, writing only constants and NOP
packets.

At xsdk `608c29f` (`probe_draw_cost_before.log`):

| Variant | Clocks a cube | us |
|---|---|---|
| The vertex constants (20 dwords with the alignment) | 594 | 0.186 |
| The pixel constants (20 dwords) | 466 | 0.146 |
| The indexed draw (9 dwords) | 384 | 0.120 |
| A cube: all three | 1,587 | 0.497 |
| 49 dwords stored straight into one reservation | 369 | 0.115 |
| The same, built on the stack and copied in | 618 | 0.194 |
| The two constants' sets through the stream | 1,200 | 0.376 |

- **Reading back what was just stored costs about 5 clocks a dword:** 249 clocks over the 49 a cube built on the
  stack and copied in, as every draw and state call did. Each constant set copied its values into the stage's kept
  copy, then read the group back out of it to write the packet.
- **A kick costs the CPU 1.1 to 1.3 us,** 3,500 to 4,100 clocks: the stream's constants cost 145 clocks a cube more
  than recording them, over 154 kicks a round, with segments of 1,024 dwords (`probe_draw_cost_in_place.log` and
  `probe_draw_cost_candidates.log` came out at 3,500 and 4,100).
- **Writing constants** (`probe_draw_cost_candidates.log`), the two sets a cube with the packets as the layer lays
  them: 32-bit stores keeping nothing 525 clocks, keeping the new values 759, and 16-byte VMX stores keeping them 511.
  The layer's own took 828 then. Each group's values start on 16 bytes, so VMX stores fit them, and they land in
  write-combined memory (see "Copies into write-combined and cached memory").
- **Each reservation through `xsdk_gpu_reserve` costs 86 clocks recording,** about 110 through the stream: the same 49
  dwords in three reservations, as a cube's three calls make them, took 544 clocks against 372 in one, and 650
  through the stream (`probe_draw_cost_reservations.log`).
- **A mistake of mine hung the console.** The three-reservation variant's first run through the stream stored
  arbitrary dwords there, which the GPU ran as packets; the console stopped answering over the network and the user
  restarted it, with no log. Every variant now writes one NOP packet, its payload after it.

## The cost of a draw, milestone 38 (2026-10-15)

docs/scope/38-draw-cost.md. What changed, from the probe's findings above:
- **Packets written in place.** The calls that run per draw reserve their exact size and write their packets into it
  (`gpu_reserve_packets`), not built on the stack and copied in: the draws, shaders, vertices, texture, blend, depth,
  raster, scissor, viewport, the cache invalidation and the stream's segments.
- **Float constants as 16-byte VMX stores,** each from the caller's values or the stage's kept copy, which takes the
  new values by the same stores and is never read back for them. The alignment assert stands guard, as `stvx` drops an
  address's low 4 bits.
- **Reservations inline.** While there's room before the segment's or block's end, or the recording's, a call takes it
  with a pointer and a compare (`gpu_reserve`); the rest go through `xsdk_gpu_reserve`. The ring's dword count is
  taken as segments go to the ring.
- **Fewer kicks.** The first segment after a fence is 1,024 dwords, as before, so the GPU starts on a frame early; each
  next is twice the last, up to 8,192.

The probe across the steps, clocks a cube (`probe_draw_cost_before.log`, `_in_place.log`, `_vmx.log`, and
`probe_draw_cost.log` at the end):

| Variant | Before | In place | VMX constants, 8,192-dword segments | Inline reservations |
|---|---|---|---|---|
| The vertex constants | 594 | 442 | 376 | 339 |
| The pixel constants | 466 | 391 | 317 | 271 |
| The indexed draw | 384 | 160 | 160 | 154 |
| A cube | 1,587 | 1,066 | 852 | 757 |
| The constants through the stream | 1,200 | 959 | 724 | 570 |
| Kicks a round, through the stream | 154 | 154 | 19 | 21 |

On the console, with xsdk at this milestone and vulkan4xenon `c1e3aaa` rebuilt on it:
- **vkbench passed 14 of 14** (`vkbench.log`), both backends drawing the same pictures. Draw and submit, us, against
  milestone 37's run (vulkan4xenon:results/2026-10-15-guide/vkbench_after_1.log):

  | Scene | xsdk before | xsdk now | vulkan4xenon before | vulkan4xenon now |
  |---|---|---|---|---|
  | draws_250 | 159 | 79 | 324 | 225 |
  | draws_1000 | 615 | 296 | 1,263 | 858 |
  | draws_4000 | 2,440 | 1,162 | 5,285 | 3,628 |
  | states_250 | 255 | 121 | 518 | 325 |
  | states_1000 | 1,003 | 463 | 2,048 | 1,271 |
  | uniforms_1000 | 613 | 295 | 715 | 570 |
  | stream_8000 | 521 | 528 | 541 | 545 |

  A cube costs the layer 0.29 us, from 0.61, still 49.1 dwords; `draws_4000` kicks 29 times a frame, from 193. The
  fill scenes' times didn't move.
- **The GPU is now behind the CPU** in the cube scenes, so more of a frame's GPU work comes after the submit:
  `draws_1000`'s 698 us from 380, `draws_4000`'s 821 from 329, `states_1000`'s 1,228 from 653. From the frame's start
  to its fence, `draws_250` takes 709 us against 714, `draws_1000` 994 against 995, `uniforms_1000` 995 against 996,
  `draws_4000` 1,983 against 2,769, and `states_1000` 1,691 against 1,656. With every segment 8,192 dwords
  (`vkbench_segments_8192.log`) the GPU started each frame later and those times ran 35 to 45 us longer; a first
  segment of 1,024 dwords took that back. The GPU's own work for a cube, `draws_1000`'s time to the fence less
  `draws_250`'s over the 750 cubes between, is about 0.38 us, more than the CPU's now (TODO.md).
- **xsdk_test passed 313 of 313** (`xsdk_test.log`). Its check that a long write runs in segments before its fence now
  follows the new sizes: 25,604 dwords ran as 5 segments. The run before failed that check, still counting on 1,024
  dwords a segment, and the controller's two, the controller not yet reconnected after the restart above
  (`xsdk_test_first.log`).
- **Broken on purpose,** each new constant written from the kept copy before it takes the new one, so every set sends
  the last one's: 303 of 313 (`xsdk_test_break_stale.log`). The 10 that failed are the tests that draw with constants
  (constants, indexed, depth, hierarchical depth, raster and microcode), and no other.
- **vulkan4xenon_test passed 112 of 112** (`vulkan4xenon_test.log`).
- **Dusklight wasn't run:** the user didn't need it played, today's goal being xsdk's own improvements.

## The GPU's cost of a draw (2026-10-15)

`build/probe_gpu_draw_cost.xex` (`tests/probe_gpu_draw_cost.c`), for milestone 39 (logs:
`results/2026-10-15-milestone-39/`). It times only the GPU.
- **The shaders** are vkbench's pair, built by `tools/gen_draw_cost_shaders.py` through xost's glslang and compiler at
  xsdk's register split: a matrix from push constants placing the cube, and the texture times a tint.
- **The batches.** Each variant's draws are recorded once, 500 cubes and 2,500. Each batch runs after a resolve has
  cleared eDRAM and the frame's state is set, and is timed from its kick to its fence, the best of 5. The difference
  over the 2,000 cubes between is a cube's cost.
- **The cubes** are 4 pixels across, one to each of a grid's cells, so pixel work stays small. After the cubes' large
  batch, 100 of the first 100 cells showed their cube in every run.

Before the milestone, at xsdk `f360ee3` (`probe_gpu_draw_cost_first.log`, `_indices.log`, `_shaders.log`,
`_front_end.log` and `_before.log` add variants in turn, each repeating the last's to within 0.001 us):

| Variant | us a cube |
|---|---|
| A cube as the layer draws it: both constant groups and the indexed draw | 0.317 |
| The draw alone | 0.293 |
| The draw alone: one triangle; one with no area; one point | 0.202; 0.203; 0.210 |
| The draw alone: its 36 corners without indices; the indices twice over | 0.293; 0.411 |
| The draw alone: no texture in the pixel shader; no texture coordinates either | 0.293; 0.293 |
| The draw alone: no depth test; no colour written | 0.293; 0.293 |
| Both constant groups, no draw | 0.112 |
| Both constant groups loaded from memory, and the draw | 0.355 |
| A cube, without the two register writes before its draw | 0.285 |
| A cube and a texture change; without the fetch constant flush | 0.381; 0.351 |
| A cube and its program set again, unchanged; and a program change | 0.427; 0.677 |
| A cube, only the pixel shader loaded again; a pixel shader change so | 0.359; 0.609 |
| vkbench's states: a texture change every draw, a program change every second, blending every fourth | 0.702 |

- **Every draw cost about 0.2 us before anything was drawn.** A point and a triangle with no area cost what a triangle
  did; the shaders' work, the indices, depth and colour cost nothing a draw. Each index added about 1.6 GPU clocks.
- **`xsdk_gpu_set_shaders` loaded both shaders every time.** Setting the program again cost 0.11 us a draw; loading
  only the pixel shader 0.07 us less than loading both.
- **The fetch constant flush after a texture isn't needed here.** Without it, cubes alternating between two textures
  every draw each showed its own (100 of the first 100 cells, `probe_gpu_draw_cost_before.log`), and a change cost
  0.03 us less.

## The GPU's cost of a draw, milestone 39 (2026-10-15)

docs/scope/39-gpu-draw-cost.md. What changed, from the probe:
- **`COHER_DEST_BASE_1` and `VGT_INDX_OFFSET` are written once, with the target,** not 0 before every draw.
- **`xsdk_gpu_set_shaders` loads only a shader that isn't loaded,** and writes nothing when neither changed. What's
  loaded is forgotten after the set-up, a resolve, a recording and a recorded buffer's execution, XAM's commands (the
  set-up again), and a shader freed or patched.
- **`xsdk_gpu_set_texture` writes no fetch constant flush.** `xsdk_gpu_set_vertices` still writes its own, untried.

The probe after (`probe_gpu_draw_cost.log`):

| Variant | Before | After |
|---|---|---|
| A cube as the layer draws it | 0.317 | 0.285 |
| The draw alone | 0.293 | 0.118 |
| The draw alone, one triangle | 0.202 | 0.048 |
| A cube and a texture change | 0.381 | 0.295 |
| A cube and its shaders set again, not their literals | | 0.285 |
| A cube and a program change | 0.677 | 0.608 |
| vkbench's states | 0.702 | 0.484 |

- **The 0.2 us a draw was the two register writes before it.** Without them the draw alone costs 0.118 us and a
  triangle 0.048: writing either register between draws held each draw until the one before had finished.
- **Constant groups between draws do the same.** A cube costs 0.285 us, where its draw alone costs 0.118 and its
  two constant groups alone 0.112. With the probe's 4-pixel cubes the wait is short; with bigger ones each draw waits
  out the last one's pixels.
- **A program change through the layer now loads only the pixel shader.** It costs 0.608 us against the raw pixel-only
  load's 0.552; the 0.056 between is the vertex shader's literal group, which vkbench's pattern, and the probe's,
  writes again at every program change.

On the console, with vulkan4xenon `c1e3aaa` rebuilt on it:
- **vkbench passed 14 of 14** (`vkbench.log`), both backends drawing the same pictures. From the frame's start to its
  fence, xsdk's `states_1000` takes 1,630 us, from milestone 38's 1,691, 3.6% less. The milestone asked for 4%; the
  user accepted 3.6% as within noise of what the target was set from. The cube scenes held at their milestone 38
  times: `draws_250` 708 us, `draws_1000` 995, `draws_4000` 1,988, `uniforms_1000` 996. Their GPU time is the
  constant groups' wait above, as their cubes cover the screen between them.
- **Broken on purpose,** `xsdk_gpu_set_shaders` looking only at the vertex shader so a new pixel shader isn't loaded
  under the same vertex shader: the microcode test, which draws each pixel shader's tile under one vertex shader,
  failed (`xsdk_test_break_vertex_only.log`). xsdk_test's other tests with more than one program change the vertex
  shader too, and passed.
- **vulkan4xenon_test passed 112 of 112** (`vulkan4xenon_test.log`).
- **xsdk_test passed 313 of 313** (`xsdk_test.log`), once the user had woken the controller; the run before failed
  only the controller's two checks, the controller asleep (`xsdk_test_controller_asleep.log`).

## How the kernel hands a title its faults, milestone 40's probe (2026-10-15)

`tests/probe_fault.c`, with its faulting instructions at known labels in `tests/probe_fault_sites.S`, ran once per mode,
the mode read from `probe_fault.txt` beside it (docs/scope/40-fault-handler.md; logs:
`results/2026-10-15-milestone-40-probe/`, one per mode). Nothing public gives `RtlSetVectoredExceptionHandler`'s
arguments or the console's register context (Xenia lists the export with no code:
xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc:891-892), so the probe found them. The console runs the debug monitor
`Xbdm.xex` ("XEX checks the kernel enforces"), which may account for what happens with no handler, and for the trap.
- **With no handler,** a store to address 0 didn't stop the title (`probe_fault_plain.log`). Its next line was logged,
  then the title ended within 1 to 2 s, and the console was back at the dashboard. A run after the handler was cleared
  did the same (`probe_fault_probe.log`). A side check on 2026-10-13 had seen `main` run again after such a store; it
  didn't happen here.
- **The hook.**
  - `RtlSetVectoredExceptionHandler` (0x36D), passed the handler as both arguments, returned 0.
  - The kernel then called the handler for every fault below, on the faulting thread, including threads placed on each
    of hardware threads 0 to 5.
  - r3 points to two words, the exception record and the register context, all three on the faulting thread's own
    stack, as Windows' `EXCEPTION_POINTERS` holds them. r4 to r6 held 0, 0x9030 and -1.
  - `RtlClearVectoredExceptionHandler` (0x36E), passed 0, returned 0, and the next fault didn't reach the handler.
- **The record** is laid out as Xenia's `X_EXCEPTION_RECORD` (xenia:src/xenia/xbox.h:314-323): the code, flags 0, no
  nested record, the faulting instruction's address, then the parameters.
  - A load or store at address 0: 0xC0000005, with 2 parameters: 0 for a read or 1 for a write, then the address
    accessed.
  - A 64-bit store 4 bytes into write-combined memory: 0x80000002, with 1 parameter, the address (0xBFC40004).
  - Primary opcode 0: 0xC000001D.
- **The context** (`probe_fault_probe.log`). Before faulting, the probe set every register it could to a pattern; this
  is where each turned up:

  | Offset | Holds |
  |---|---|
  | 0x000 | 0x17, the context's flags as Windows keeps them first (unverified) |
  | 0x004 | 0xB030, likely MSR (unverified) |
  | 0x008 | IAR |
  | 0x00C | LR |
  | 0x010 | CTR, 64 bits |
  | 0x018 | r0 to r31, 64 bits each, to 0x117 |
  | 0x118 | CR |
  | 0x11C | XER |
  | 0x120 | 0xFFF80000_00000000, likely FPSCR held as a double (unverified) |
  | 0x128 | f0 to f31, 64 bits each, to 0x227 |

  The vector registers weren't probed.
- **Resuming.** The handler returned -1, Windows' "continue execution", with IAR moved 4 past the faulting instruction.
  Each fault then resumed at the next instruction, with r14-r31 and f14-f31 whole. 13 faults resumed in one run,
  one of them while presenting: 60 frames of 60 shown, and the GPU closed after.
- **The stack.** Walking the back chain from the context's r1, the slot 4 bytes into each caller's frame held xsdk's
  return addresses, both of the two checked. Above them, the kernel's frames that started the title held one at 8
  bytes below the next frame, 0x80075E5C.
- **A trap** (`tw 31,0,0`, which `__builtin_trap` emits) never reached the handler, and the title carried on past it.
  The debug monitor may take it as a breakpoint; a console without one may differ. Xenia treats `twi 31,r0,N` as special
  traps, 20 a debug print and 22 a break (xenia:src/xenia/cpu/backend/x64/x64_emitter.cc:371-380).
- **A stack overflow hangs the console, with a handler or without.** A thread with a 64 KB stack, 0x70120000 to
  0x70130000, recursed a kilobyte a call. It logged depth 48 at r1 = 0x70122B50, then nothing. The handler never logged,
  and the console stopped answering until the user restarted it, in both runs (`probe_fault_overflow.log`,
  `probe_fault_plain_overflow.log`, each fetched after the restart). The record and context go on the faulting thread's
  own stack, so an overflowed stack leaves the kernel nowhere to put them.
- **Ending from the handler.** `XamLoaderTerminateTitle`, called in the handler with the GPU and display open and frames
  presenting, took the console back to the dashboard within about 2 s (`probe_fault_end.log`).

So milestone 40's handler:
- sets itself with `RtlSetVectoredExceptionHandler`;
- reads the record and context through the pointers in r3, at the offsets above;
- ends the title with `XamLoaderTerminateTitle`.

It can't catch a stack overflow, and on this console it doesn't see traps.

## A pixel shader's depth, vulkan4xenon milestone 18's step 3 (2026-10-16)

Aurora's Z textures write each pixel's depth from a texel (vulkan4xenon:docs/ROADMAP.md, "Milestone 18"), so xost
exports `gl_FragDepth` and xsdk has to know which draws do. Xenia takes a pixel shader's depth from export 61's x
(xenia:src/xenia/gpu/shader_translator.cc:1337-1338, dxbc_shader_translator.cc:1591-1594) and never reads
`SQ_PROGRAM_CNTL`'s ps_export_mode (registers.h:145), which xsdk had at libxenon's 2 for every pixel shader
(libxenon:drivers/xenos/xe.c:419). `tests/probe_depth_export.c` drew with `tools/gen_hiz_shaders.py`'s depth shader,
the interpolated colour to `oC0` and c0.x to `oDepth.x___`, at mode 2 and at 3, each draw after a resolve clearing
colour and depth to 1 (log: `results/2026-10-16-depth-export/probe_depth_export.log`).
- **Mode 2 drew nothing.** A full-screen draw at 0.25 writing 0.75, always, then a plain one at 0.5, less-than: of
  921600 pixels, none held either draw's colour, so the plain draw failed everywhere too. Behind a near quad at 0.25,
  the depth shader's draw at 0.75 writing 0.1 showed nowhere, the hierarchical buffer on or off.
- **Mode 3 exports the depth too.** The plain draw at 0.5 showed on every pixel, over the 0.75 written.
- **The hierarchical buffer tests a draw by its polygon's depth, not the one its pixel shader writes.** Behind the near
  quad, the draw at 0.75 writing 0.1, less-than, showed on 162 pixels with the buffer as xsdk set it and on all 921600
  with it off.

So xost gives a pixel shader that writes `gl_FragDepth` ps_export_mode 3, and `xsdk_gpu_set_shaders` reads the mode's
low bit from the pixel shader's record: while it's set, `RB_HIZCONTROL` is 0, and the buffer is kept for the draws
after, as such a draw's depth still passes the depth test and stays within the buffer's bound.

With that, on the console (`results/2026-10-16-depth-export/`):
- **xsdk_test passed 316 of 316** with the user at the pad (`xsdk_test_pad.log`), and 314 without, the two failures
  the controller's (`xsdk_test.log`). Its new check drew, over clears to 1 and less-than, the depth shader at 0.75
  writing 0.1 behind a near quad, and at 0.2 writing 0.8 with a quad at 0.5 drawn over it: of 921600 pixels, none hid
  the first or failed the second.
- **Broken on purpose,** the buffer's control ignoring the shader, or `xsdk_gpu_set_shaders` leaving it as the depth
  setting left it, 921438 pixels hid the first draw each time and the check failed (`xsdk_test_hiz_test_broken.log`,
  `xsdk_test_hiz_put_broken.log`). The quad at 0.5 still showed everywhere, though the buffer wrote during the draw
  from 0.2: it didn't keep that polygon's depth.

## A fifth vertex fetch (2026-10-16)

Melee's fighters drew with spikes on the console. Two frames were dumped from a diagnostic build, each GX draw as the
GPU got it: its decoded vertices, indices, uniform pieces and GLSL. Off the console, the vertices transformed as the
GLSL says, and xost's microcode run on its simulator, put every vertex where it belonged. Replayed through vulkan4xenon
alone, each draw white over black on the console, the draws out of place were the ones whose vertex shader fetched
five attributes in one exec: 96 of the 99 that drew, against none of the 68 with four or fewer
(`results/2026-10-16-fifth-vertex-fetch/replay_dump1_before.log`, `replay_dump2_before.log`).
- **The exec's vertex cache bits.** An exec has six bits Xenia calls "vertex cache?", four in vc_hi at the top of its
  first word and two in vc_lo at the bottom of its second (xenia:src/xenia/gpu/ucode.h:243-246). xost set a mask of
  its fetch count capped at four, and vc_lo 0, so a fifth or sixth fetch went unmarked. In these programs the fifth
  fetch was the position.
- **Marked, the fetch is right.** With the mask over all six bits, the fifth and sixth fetch in vc_lo's bits 0 and 1,
  every draw of both frames that drew stayed in place (`replay_dump1_after.log`, `replay_dump2_after.log`), and
  Melee's fighters drew whole (`melee.log`). Which instruction each bit stands for in other orders, with fetches after
  ALU instructions, isn't known; xost's vertex shaders fetch first.
- **Xenos draws past a viewport as far as the scissor.** One draw, a backdrop across the camera, filled all 1280
  columns of the replay's target where its viewport was 1168 wide (`replay_dump1_layer_before.log`). vulkan4xenon now
  stops its scissor at the viewport (vulkan4xenon:docs/SUPPORT.md, Pipelines), and the draw covered exactly the 840960
  pixels its triangles clipped to the viewport do (`replay_dump1_layer_after.log`).
- **xsdk_test passed 316 of 316** with the mask over all six bits (`xsdk_test.log`).

## Blend constants, vulkan4xenon milestone 19's step 3 (2026-10-16)

Vulkan's blend constants take Xenos's constant blend factors, `kConstantColor` to `kOneMinusConstantAlpha`
(xenia:src/xenia/gpu/xenos.h:719-722), and the colour they read, `RB_BLEND_RED` to `RB_BLEND_ALPHA`, four floats from
0x2105 (xenia:src/xenia/gpu/register_table.inc:506-509), which Xenia hands D3D12 as its blend factor
(xenia:src/xenia/gpu/d3d12/d3d12_command_processor.cc:2937-2948). xsdk_test's raster test draws two cells by them
(`results/2026-10-16-blend-constants/`):
- **By a colour's channels:** source times (0.25, 0.5, 0.75) plus destination times one less them came out within a
  step of the CPU's sums, red to blue as the registers run.
- **By its alpha:** source times 0.6 plus destination times 0.4, the colour channels 0, so a draw reading the colour in
  place of the alpha would have shown the destination alone.
- **Each draw's own:** the constants were set before each of the two draws, and each blended by its own.
- **Written alpha first,** the four registers reversed, both cells missed and every other point held
  (`xsdk_test_s3_broken.log`). xsdk_test passed 318 of 318 with the user at the pad (`xsdk_test.log`).

## Queries and events, vulkan4xenon milestone 19's step 6 (2026-10-16)

`probe_queries.c` asked what Vulkan's events, time stamps and occlusion queries need of the command processor, in six
runs (`results/2026-10-16-queries-probe/`, a log a run):
- **The clock's high word counts.** With the counters on as before ("A clock the GPU and the CPU both read"),
  `RBBM_PERFCOUNTER1_HI` read 0, then 1 once the low word wrapped 8.8 s later (`probe_queries_steps.log`). The low
  word ignores the CPU's writes: set to 0xFFFFF000, it read 0x00264397, counting on from where it was. xsdk turns the
  counter on at set-up and puts it back at close; `xsdk_gpu_write_clock` copies the high word, the low word and the high
  word again, so a wrap between the copies shows.
- **The clock counts 498.75 a microsecond,** ten times the time base's 49.875 MHz ("The time base runs at 49.875
  MHz"). xsdk_test's two copies 100 ms apart by the CPU's clock gave 498.751 and 498.746
  (`xsdk_test_check_at_498.9.log`, `xsdk_test.log`), and vulkan4xenon's two time stamps 52 ms apart 498.74
  (vulkan4xenon:results/2026-10-16-milestone-19-step-6/vulkan4xenon_test_period_2.004.log). The 498.9 of "A clock
  the GPU and the CPU both read" came from 1 ms windows between two register reads.
- **xsdk_test passed 321 of 321** with the user at the pad, its three checks of these functions new and the fence-wait
  check moved onto the clock's (`xsdk_test.log`). Broken on purpose, a word written one more than asked, a wait that
  always passes and the low word copied for the high ones (`xsdk_test_breaks.py`), the three failed, and the fence-wait
  check, which times waits by the clock (`xsdk_test_s6_broken.log`).
- **The command processor waits on memory the CPU writes.** `PM4_WAIT_REG_MEM` with its memory bit (0x10) and the
  equal function, read through the 8-in-32 swap, held the fence after it while the word was 0 and passed 1 to 2 us
  after the CPU stored 1, after 50 ms and after 2 s alike; read without the swap, the CPU's 1 didn't match
  (`probe_queries_steps.log`). That's `xsdk_gpu_wait_word`; `xsdk_gpu_write_word` writes a word as fences do.
- **`ZPASS_DONE` wedges the GPU, and nothing tried wrote a sample count.** Each run filled the counts' memory with
  D3D's 0xFFFFFEED (xenia:src/xenia/gpu/command_processor.cc:1252-1270), set `RB_SAMPLE_COUNT_ADDR` (0x2325) to it, and
  sent the event: the fence after the event landed with the memory unwritten, and the next draw never finished. A draw
  after the address alone was fine (`probe_queries_isolated.log`). What hung it:
  - `PM4_EVENT_WRITE_ZPD` (0x5B) with `ZPASS_DONE` (21) (xenia:src/xenia/gpu/xenos.h:1559, 1598), as D3D sends it
    for a query's two records (LostOdysseyRecomp:docs/notes/occlusion-queries.md): with `RB_SAMPLE_COUNT_CTL` as the
    console leaves it, 2 (`probe_queries_hang.log`, `probe_queries_steps.log`), and at 0 (`probe_queries_controls.log`);
  - Adreno 3xx's sequence, from a capture of Qualcomm's driver: the address, the control's COPY bit (bit 1, RESET
    bit 0; Linux:drivers/gpu/drm/msm/registers/adreno/a3xx.xml), a draw of no points, then the event
    (freedreno:fdre-a3xx/freedreno.c:1113-1127), with `PM4_EVENT_WRITE_ZPD` (`probe_queries_sequence.log`) and with
    `PM4_EVENT_WRITE` (0x46) (`probe_queries_event_write.log`).
  The command processor had read everything the ring held, and `RBBM_STATUS` read 0xE8A50110: the render backend,
  shader export and memory hub busy, the vertex, primitive and scan-converter units idle
  (Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml, `RBBM_STATUS`). So the counts' write to memory is what never
  finishes. Left untried: the RESET bit, a resolve between the event and the next draw, and the rest of the state the
  console leaves before xsdk's set-up. vulkan4xenon refuses occlusion queries until a sequence that writes the counts
  is found (TODO.md).

## Texture and vertex formats, vulkan4xenon milestone 20 (2026-10-16)

The formats vulkan4xenon milestone 20 added, on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-20/`;
vulkan4xenon:results/2026-10-16-milestone-20/):
- **Every format's channels as written:** xsdk_test's formats test drew all 24 formats, DXN, DXT5A, the 16-bit fixed
  point and 10-11-11 ones new, with no wrong point in 59 frames; the largest error in a channel was 4, DXT5's, DXN's
  and DXT5A's, against the CPU's decoding. Each of vulkan4xenon_test's 47 Vulkan formats, sampled nearest, held every
  texel's channels, BC's blocks swapped 8 in 16 on their way in as the layer copies them.
- **Signed and gamma reads:** TextureSign 1 read 8-, 10- and 16-bit channels as two's complement from -1 to 1, the most
  negative as -1, and 3 read colours through Xenos's piecewise-linear curve (xenia:src/xenia/gpu/xenos.cc:27), each
  as the CPU worked it out. Filtered, an sRGB image blended its texels after the curve, which is Vulkan's order.
- **BC2 and BC3 colour blocks** whose first endpoint isn't above the second still took four colours, as Vulkan's do.
- **A vertex attribute's components picked** in the fetch's destination swizzle (`XsdkVertexElement`'s `components`)
  came out in place, in xsdk_test's four cells and vulkan4xenon_test's 19 vertex formats.
- **Linear filtering of 10-10-10-2 and 16-bit floats is wrong.** Halfway between two texels, every 8-bit, 16-bit fixed
  point and BC format gave their mean within the test's tolerance, but `A2B10G10R10` and `A2R10G10B10`, unsigned and
  signed, missed all 360 of their probes over 30 frames and the 16-bit floats 270 to 360: one point that should have
  held 66 33 88 held 06 B3 0A (`vulkan4xenon_test_filtering_counts.log`). Sampled nearest, every one of them held its
  texels. The cause isn't known. Untried: the formats Xenos has for filtering at more precision,
  `k_2_10_10_10_AS_16_16_16_16` and `k_16_EXPAND` to `k_16_16_16_16_EXPAND` (xenia:src/xenia/gpu/xenos.h:503-505, 530).
  vulkan4xenon samples them nearest (TODO.md).

## Branches, vulkan4xenon milestone 21 (2026-10-16)

xost's milestone 21 lowering on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-21/`;
vulkan4xenon:results/2026-10-16-milestone-21/):
- **An if/else on data merged by selects** drew right in vertex shaders, nested and with an `&&`, in an unrolled loop's
  body, and around `gl_FragDepth`, compiled on the PC and on the console.
- **Execs on the predicate hung the GPU in a vertex shader.** An if/else on a push constant, its arms in a `setp`'s
  `(p0)` and `(!p0)` execs as Xenia describes them (xenia:src/xenia/gpu/ucode.h:114-130), placed its first frame's
  quads wrong and then the GPU didn't go idle (0xC00000B5); every check after failed, and the title hung as it ended
  (`vulkan4xenon_test_vertex_predicate_hang.log`). The same lowering in a pixel shader, an if/else on a push constant
  inside an unrolled loop's body, drew right. Xenia gives no difference between the stages, so the cause isn't known;
  xost keeps a vertex shader's if/else on a uniform as selects, as on data.
- **Hardware loops a block counts:** a pixel shader's loop counted by a push constant ran 0, 1, 3 and 6 passes, 300
  as 255 and -5 as none, clamped by vulkan4xenon; one counted by a uniform buffer broke where its if/else on data
  said; a vertex shader's counted by a dynamic uniform buffer ran its count at each draw's offset; and a vertex and a
  pixel shader each ran their own count at once, the vertex shader's loop constant 0 and the pixel shader's 16.
  Writing all 40 boolean and loop words between draws, where a count changed, didn't hang.

## LODs, facing and vertex textures, vulkan4xenon milestone 22 (2026-10-16)

xost's milestone 22 lowering on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-22/`;
vulkan4xenon:results/2026-10-16-milestone-22/), each drawn over an image whose levels are each a colour:
- **A LOD, gradients or a bias the shader works out** took the level Xenia's model says, in eight bands each:
  `setTexLOD` with the fetch's computed LOD off for `textureLod`, `setGradientH` and `setGradientV` with
  UseRegisterGradients for `textureGrad`, and `setTexLOD` added to the computed LOD for `texture()`'s bias; a constant
  LOD of 1.25 and 6.25 in the fetch's LOD bias field took levels 1 and 6. Each band was one LOD, so the pixel groups in
  flight agreed; where they don't, nothing may wait between a setter and its fetch ("Loops on the console, milestone
  46 step 1").
- **`setTexLOD`'s value held across execs:** a vertex shader whose setter and fetch sit in different execs sampled the
  level it set.
- **`gl_FrontFacing`,** the sign of the pixel parameters' x, held on the face the pipeline's front face makes front,
  clockwise and counter-clockwise, and a `discard` on it with no interpolator, the parameters in r0, kept the front
  face alone.
- **A vertex shader's texture fetches,** at the LOD it gives, with FetchValidOnly off and no vertex cache bits, read
  the same 32 fetch constants as a pixel shader's: two textures moved a column of vertices and picked its level.
  `xsdk_shader_move_textures` moved a fetch's constant to slot 5 and back, and vulkan4xenon's vertex samplers drew
  from the slots after the fragment shader's.
- **One launch froze the console** before the title opened its log, the run before it clean; three runs after a
  reboot passed, so it's not known to be the title's.

## Cube and 3D textures and the rest of addressing, vulkan4xenon milestone 23 (2026-10-16)

xsdk_test on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-23/`), its draws through shaders xost
compiled, so its cube lowering and 3D fetch with them:
- **Cube textures,** laid out as Xenia has them (xenia:src/xenia/gpu/texture_util.cc:209-449): a level's six faces
  in turn, each a page apart, the mips after the base's six faces, each face's packed tail its own. A 32x32 cube at
  levels 0 and 1 and an 8x8 one at levels 0 and 2, from each face's packed tail, showed each face where Vulkan's
  directions point, +x, -x, +y, -y, +z then -z, through xost's cube op, its 1/|ma| and its 1.5 bias.
- **3D textures,** tiled 32 by 32 by 4 slices, the bank flipping every 4 slices: a 32x32x8 texture showed its eight
  slices at their w, and an 8x8x3 one its three from its packed tail. Along w it wrapped, clamped, and filtered
  linearly halfway between two slices, the volume filters set in the fetch constant's dword 4.
- **Every face and slice read back** as uploaded, a slice at a time.
- **ClampMode 3 to 7** worked as worked out: mirror once, the halfway modes, and the border, black or white; the
  black border's alpha is 0, as Xenia's backends take it (xenia:src/xenia/gpu/vulkan/vulkan_texture_cache.cc:817-824).
- **Broken on purpose,** a face's stride worked out for seven faces, 3D's bank never flipped, the border colour left
  out, downloads always from slice 0, the volume filters nearest, every border white and w's addressing dropped each
  failed the checks they aim at (`xsdk_test_broken_a.log` to `_c`). xsdk_test passed 343 of 345, the two left the
  controller's, no button pressed.

## Instances and long strips, vulkan4xenon milestone 24 (2026-10-16)

vulkan4xenon_test on the zero-fuse console (vulkan4xenon:results/2026-10-16-milestone-24/), each instance a draw of
its own:
- **A vertex fetch with a stride of 0** read the same element for every vertex: attributes at the instance's rate,
  their stream pointed at the instance's element before each draw, coloured and placed every instance of `vkCmdDraw`,
  `vkCmdDrawIndexed` and both indirect draws, `firstInstance` included.
- **Fetch constants and float constants written between back-to-back draws** took effect on the next draw: each
  instance took its own element and its own `gl_InstanceIndex`, the float constant xost lowers it to, and a shader
  without it was placed by its attributes alone, so no vertex shader's results carried over from the draw before.
- **Triangle strips wind as Vulkan's do,** and a strip's parity starts again with each draw packet: a strip of 70,000
  vertices, back faces culled, drawn in pieces of 65,534 sharing their last two vertices, the pieces an even number
  on, showed its first quad, the quad over the split and its last; from 32-bit indices too. A line strip's pieces,
  sharing a vertex, drew the lines over the split and its last two, their ends clipped past clip space.
- **Broken on purpose,** instance streams fetched at their real stride, the index never written, an odd step, pieces
  sharing nothing and the first piece alone each failed the checks they aim at.

## Float, 10-bit and 64-bit targets and four at once, vulkan4xenon milestone 25 (2026-10-16)

xsdk_test on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-25/`), each target resolved into a
texture of the format it converts to and every texel read back by value. The first run failed eight checks
(`xsdk_test_mode_2.log`), which settled two of the milestone's questions:
- **`ps_export_mode` counts the colours a pixel shader exports,** from bit 1, depth in bit 0, as R600's
  `SQ_PGM_EXPORTS_PS` does (radeonhd:src/r600_exa.c:164, one colour 2). A shader exporting `oC0` to `oC3` at
  libxenon's 2 left targets 1 to 3 at their clears, and target 0 held 1 1 0.2 0.25 where both draws should have left
  1 0.8 0.4 1. At 8, each target took its own export. Xenia never reads the mode (xenia:src/xenia/gpu/registers.h:145).
  xost now writes the count from `oC0` to the highest output, so one colour stays 2 and one with depth 3, as before.
- **A 64-bit target's clear takes its red and green from `RB_COLOR_CLEAR_LO`** and its blue and alpha from
  `RB_COLOR_CLEAR`, the other way round from Xenia (xenia:src/xenia/gpu/draw_util.h:565-567): 16_16_16_16 and
  16_16_16_16_FLOAT cleared to 0.25 0.5 0.75 1 read back 0.75 1 0.25 0.5.

With both fixed (`xsdk_test_alpha_quarter.log`, then `xsdk_test.log`):
- **Every format held its clear and a quad's colour,** each channel in place: 2_10_10_10, its 7e3 float, 16_16_16_16
  with its sign, 16_16_FLOAT, 16_16_16_16_FLOAT and 32_FLOAT, past 1 and below 0, resolved into A2B10G10R10, R16G16F,
  R32F or R16G16B16A16F with `RB_COPY_DEST_INFO`'s float number format for the floats.
- **Which targets blend,** a quad added onto itself: 2_10_10_10, 7e3 and 16_16_16_16 doubled every texel.
  16_16_FLOAT and 16_16_16_16_FLOAT doubled none, 1.5 and 1000 coming out 65536 and 131008, and 32_FLOAT gave NaN,
  so the float targets don't blend.
- **A 2_10_10_10 target rounds the source to its own bits before it blends:** `oC1` at alpha 0.25 blended by its
  alpha came out 0.533 0.733 0.333 where 0.6 0.7 0.35 was worked out, which is the blend at 1/3, 0.25 in 2 bits. The
  check now blends at 1/3, which 2 bits hold.
- **Four targets at once:** 8_8_8_8, 2_10_10_10, 16_16_16_16_FLOAT and 32_FLOAT, set in `RB_COLOR_INFO` and
  `RB_COLOR1_INFO` to `RB_COLOR3_INFO` in one packet, a 64-bit target taking twice a 32-bit one's tiles, each blended
  by its own `RB_BLENDCONTROL` and masked by its own four bits of `RB_COLOR_MASK`, and each resolved by pointing
  `RB_COLOR_INFO` at it.
- **Broken on purpose,** each of the five sets failed the checks `xsdk_test_breaks.py` names for it and only those
  (`xsdk_test_broken_a.log` to `_e`). xsdk_test passed 359 of 359, a button pressed. Its texel compare had taken a NaN
  as near, so 32_FLOAT's blend line counted every texel doubled; it no longer does.

## Raster state, vulkan4xenon milestone 26 (2026-10-16)

xsdk_test on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-26/`):
- **Two-sided stencil works as Xenia reads it** (xenia:src/xenia/gpu/registers.h:799-834): with `backface_enable` set,
  back faces took `RB_DEPTHCONTROL`'s `_bf` fields and `RB_STENCILREFMASK_BF`, the two-sided cell's resolved words
  holding 0x11 for the front face's mark and 0x22 for the back's.
- **User clip planes clip in clip space,** keeping where dot(the exported position, plane) >= 0, as Xenia applies them
  (dxbc_shader_translator.cc:1033-1053): `PA_CL_CLIP_CNTL`'s `ucp_ena` bits and `PA_CL_UCP_0_X` to `PA_CL_UCP_5_W`
  cut planes 0, 3 and 5's cells at their lines, and a target set left nothing cut after.
- **A point's size is its full width in pixels from `oPts.x`,** sprites of 24 and 64 pixels as wide as that, with
  `PA_SU_POINT_MINMAX` at radii 0 to 0xFFFF; `gl_PointCoord`, the pixel parameters' |zw|, ran 0 to 1 from the
  sprite's top left. **The console needs `vs_export_mode` 2 for it:** the break leaving the mode 0 failed the sprite
  check as well as the compiler's (`xsdk_test_broken_s3c.log`), which Xenia, not reading the mode, couldn't show.
- **Wireframe and point fill:** dual mode with both polygon types lines, or points, drew a quad as its triangles'
  edges, or its corners, and nothing inside. The first run failed it (`xsdk_test_fill_unsigned.log`): the test placed
  the quad with `-FILL_HEIGHT`, an enum constant made unsigned by `CLEAR_ARGB` past `INT_MAX`, so its bottom corners
  went far off screen and only the top edge and corners drew (`xsdk_test_fill_constants.log` logs the constant,
  0x4B360B61 for about -0.22), found against a throwaway probe that drew the same quad right in each state the test
  sets. With `-(FILL_HEIGHT / (HEIGHT / 2.0f))` the check passes.
- **Broken on purpose,** each of the nine sets failed the checks `breaks.py` names for it (`xsdk_test_broken_s1a.log`
  to `_s4b`). xsdk_test passed 364 of 366, the two left the controller's, no button pressed.

## Resolving part of a target, vulkan4xenon milestone 27 (2026-10-17)

xsdk_test on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-27/`): a resolve keeps to its drawn
rectangle and its window scissor, as Xenia has it (xenia:src/xenia/gpu/xenos.h:911-984). Rectangles of four quarters
meeting off the grid in a 204x116 target, one inside and one reaching the right and bottom edges, resolved into a
texture of sentinels into the same texels and no others, 0 texels wrong, and each resolve cleared what it copied
alone; past an edge off the grid it wrote only the texture's padding. A depth clear of a rectangle cleared its depth
alone, and a quad between the two depths showed in the rectangle alone, hierarchical depth turned off after it. Broken
on purpose, the rectangle at the corner, the whole target resolved, and hierarchical depth left on each failed the
checks they aim at (`xsdk_test_broken_a.log` to `_c`). xsdk_test passed 373 of 373, a button pressed.

vulkan4xenon's render areas and blits drawn on the GPU, through it, passed too (vulkan4xenon:docs/ROADMAP.md,
milestone 27): a replay with a blit drawn took 87 us of the CPU against 24816 us with it on the CPU, its submission
done after 9.8 ms against 25.9 ms.

## Occlusion queries, vulkan4xenon milestone 28 (2026-10-17)

`tests/probe_occlusion.c` on the zero-fuse console (`results/2026-10-16-vulkan4xenon-milestone-28/`), each try from a
fresh boot, a 256x128 quad of 32768 pixels and one hidden behind it drawn once to count them, then with the try's
counting around each. None got sample counts:
- **`registers`:** Yamato's `RB_TOTAL_SAMPLES` to `RB_SFAIL_SAMPLES`, 0x0F0F to 0x0F12
  (amd-gpu:include/reg/yamato/22/yamato_offset.h:578-581), read 00000000, 0000001A, 00001041 and A5A5A5A5 before and
  after every draw, by the CPU and through `PM4_REG_TO_MEM` alike: not counters, or not counting.
- **`viz_query`:** `PM4_VIZ_QUERY` around a draw with `PA_SC_VIZ_QUERY` on set its query's bit of
  `PA_SC_VIZ_QUERY_STATUS_0` for the quad, for the quad hidden behind it too, and for one with the register's
  `KILL_PIX_POST_HI_Z`, which then drew nothing, and left it clear for an empty query. So it answers whether a draw got
  past rasterizing and the coarse hierarchical test, not whether a sample passed the depth test, and Vulkan's occlusion
  query can't take it as a count. It didn't hang the GPU. The command processor's copy of the status word at the
  query's end held the bits from before it, and its copy once the GPU was idle the query's.
- **Every try that sent `ZPASS_DONE` hung at its first one,** its own fence landing and the wait for idle never ending,
  `RBBM_STATUS` E004450C, as the six runs before (see "Queries and events"): `RB_SAMPLE_COUNT_CTL`'s RESET at a first
  event, alone or with COPY at each; the `RST_PIX_CNT` event first; the resolve's cache flush or a whole resolve after
  each; `RB_BC_CONTROL`'s `DISABLE_SAMPLE_COUNTERS` set, so the event hangs without the counters too; and the registers
  xsdk's set-up writes put back as the dashboard leaves them before each event, all of them and each of seven groups,
  of which only `CP_PERFMON_CNTL` differed, 0 against xsdk's 1.

So sample counts stay out of reach, and nothing public is left to try. The user took the visibility query for
vulkan4xenon's occlusion queries all the same, a deviation without `occlusionQueryPrecise`: a query answers 1 if its
draws rasterized anything (`xsdk_gpu_begin_visibility`; vulkan4xenon:docs/SUPPORT.md, Queries).

`xsdk_gpu_begin_visibility` and `xsdk_gpu_end_visibility` then ran in xsdk_test (`xsdk_test.log` in the same folder),
each status word copied once the GPU was idle:
- **A query's end writes its result, set or clear:** id 1, begun again around no draw after its quad had set it, read
  clear (0x2, then 0x8 once ids 3 and 1 had run), so an id can be reused once its last result is copied.
- **Ids 32 to 63 answer in `PA_SC_VIZ_QUERY_STATUS_1`,** id 63's quad setting its bit 31 and id 32's no draw leaving its
  bit 0 clear.
- **`PA_SC_VIZ_QUERY`'s id picks the bit the draws set, not the packet's:** with the id written unshifted, over the
  enable bit, a query whose packet named id 1 set bit 0, the id the register then held (`xsdk_test_broken_c.log`), where
  Xenia sets the packet's (xenia:src/xenia/gpu/command_processor.cc:1622-1630). xsdk writes the same id in both.
- **The status words outlast a title:** the broken runs began with bits 0 and 3 set, left by an earlier run, which is
  why the break sets don't list the checks such a bit could pass.
- Through vulkan4xenon, a quad off screen and one culled left their bits clear, and `vkCmdClearAttachments` ended and
  began its query's id around itself so its rectangle isn't counted (vulkan4xenon:docs/ROADMAP.md, milestone 28).

Broken on purpose, the copy without the idle wait, every id read from the first word and the id unshifted each failed
the checks `xsdk_test_breaks.py` names (`xsdk_test_broken_a.log` to `_c`). xsdk_test passed 380 of 382, the two left
the controller's, no button pressed.

## a0 doesn't survive an alloc (2026-10-17)

`tests/probe_relative_constants.c` draws vertex shaders `tools/gen_relative_constant_probe.py` builds by hand, each
loading a0 with xost's `maxasf` from a vertex input and reading `c[k+a0]`; c[n] draws as red n, green 255 - n and blue
n ^ 0x5A, so a pixel names the constant read (`results/2026-10-17-milestone-45/`). Its first run,
`probe_relative_constants.log`, drew 39 shapes over four quads each, a0 the same in all four and then 3, 7, 12 and 18.
Its stress run, `probe_relative_constants_stress.log`, draws ten shapes as one draw each of a 64 by 36 grid of 20-pixel
quads, 13824 vertices, quad i's a0 2 + i % 97, every quad's centre checked: 20 draws of each under three loads, the
plain pixel shader, one of 120 ALU instructions before its export, and that with four full-screen draws before the grid.
- **a0 is lost at any control flow `alloc`.** Read after the interpolators' alloc with a0 loaded before it, all 46,080
  quads of each load were wrong, reading no constant; so were the first run's reads after the position's alloc (case
  13). The wrong reads are a0's, not the instruction's: vector-only or co-issued with an `rcp`, src3 a temp or a
  constant, a `mul` with the constant either operand, all failed (cases 4 to 7, 9).
- **Remedies that don't hold.** One, two or four throwaway reads before the read, `max rN._, c[k+a0], c[k+a0]` with
  write mask 0, and the read done twice, passed in the first run's light draws of four quads (cases 10, 12 and 15), and
  failed in every draw of the stress run under every load: 69 to 87% of quads wrong, most reading another vertex's
  row, some no constant at all.
- **What holds:** a0 loaded again after the alloc with the same `maxasf` of the same input, right after the alloc or in
  the read's exec with an exec of other work between; the read moved before the alloc, into a temp; and reads in the
  alloc region a0 was loaded in, any number of instructions or execs later. Each passed all 46,080 quads under every
  load, and the first run's cases 1 to 3, 11 and 14 too. An export between the load and the read, no alloc between,
  doesn't matter (case 14).
- **Dusklight:** `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert` load a0 after the position's alloc for a skinned
  vertex's matrix, `c[120..122+a0]`, and read its normal matrix, `c[152..154+a0]`, for the lighting after the
  interpolators' alloc, so skinned characters were lit from wrong rows. Case 8 replays 93d36's instructions from its
  `maxasf` to its `c[152+a0]` read as xost wrote them before milestone 45 and at its step 4, at both register counts and
  with the lighting's instructions as fillers: none read the row, and the two read different wrong values.
- **xost now** loads a0 again after an alloc before a read at a0 that needs it (`set_a0`, `src/xsc_lower_block.c`): the
  lowering keeps the instruction that loaded a0 and the program's alloc count then (`xsdk_xsc_program_alloc`,
  `src/xsc_program.c`), and a read at the same index with an alloc since emits that instruction again first, in the
  read's alloc region, right before it. Every alloc kind xost emits counts, position, interpolators and colours, in
  vertex and pixel shaders. It goes in before register allocation, so the allocator keeps the load's source live to it,
  and the optimiser moves no read at a0 past an a0 load, nor, since milestone 48 step 6, past an alloc (`same_values`,
  `src/xsc_optimize.c`). Over milestone 45's corpus only those two Dusklight shaders take one,
  an ALU instruction more each and no register more, besides the test shader below.
- **The throwaway was wrong** (`b6b9004`, withdrawn): the first run read the row after one, and it took that as the
  first read after an alloc writing nothing. Across 2304 quads a draw, a0 itself is gone, and xsdk_test's check of it,
  two quads, failed in about 4 of 59 frames.
- **Xenia doesn't show it:** its a0 changes only at `maxa`, `maxas` and `maxasf`
  (xenia:src/xenia/gpu/shader_interpreter.cc:600-602, 808-823), a constant at a0 is read from it whatever came between
  (260-266), and an alloc only opens an export there (242-249) and matters to its SPIR-V translator only for memory
  export (xenia:src/xenia/gpu/spirv_shader_translator.cc:1125-1131). xost's simulator (`xdis/equivalence.py`) modelled
  a0 the same way, so no check on the PC could see it; since milestone 48 step 6 it and `xdis/simulate.py` lose a0 at
  an alloc.
- **Not tried:** reads at the loop index, aL, after an alloc. xost reads at aL only in `xsdk_xsc_lower_real_loop`'s
  loop (`src/xsc_lower_loop.c`), before the colours' alloc, so nothing it compiles does.

On the console on 2026-10-17, xsdk_test's raster test draws xost's `vk_dynamic_interpolator.vert`, compiled there,
over a grid of 560 quads in one draw, each its own index of 16, the colour read at a0 past the interpolators' alloc:
0 of 33,040 quads wrong over 59 frames, in each of two runs; with the reload off, 33,040 of 33,040. Dusklight's
skinned models, read at `c[152..154+a0]` past the interpolators' alloc, are lit from their own rows for the first
time.

## a0 through fetch grouping (2026-10-17)

A compiler bug in xost, found while the reload above was built, now a rule xost keeps. Built and run on the console on
2026-10-17.
- **Hardware fact, Xenia's:** a0 changes only at `maxa`, `maxas` and `maxasf`, and a read at a0 takes what a0 holds
  when the read runs (xenia:src/xenia/gpu/shader_interpreter.cc:260-266, 808-823). No register links the read to the
  load before it.
- **Compiler bug:** a pixel shader's fetch grouping (`ordered`, `src/xsc_program.c`) took the work a sample's
  coordinate needs up before the rest, and the a0 load that work reads at isn't among it. xost's
  `vk_dynamic_texcoord.frag` samples at `coords[i]` scaled by `1 / scales[j].w`, read first: its read of `coords[i]`
  went up past i's load, which landed after the sample, and read at j, every index wrong on xost's simulator. No
  program of milestone 45's corpus or Dusklight's pack had that shape.
- **The rule:** a read at a0 stays after every a0 load before it, and a load after every read and load before it, in
  any shader the grouping lays out, as the optimiser moves no read at a0 past an a0 load (`fixed` and `same_values`,
  `src/xsc_optimize.c`).
- **Checks:** `tests/test_dynamic_index.py` runs `vk_dynamic_texcoord.frag` on `xdis/simulate.py` for each of 16
  indices, optimiser off and on. xsdk_test's raster test compiles it on the console and draws it as one grid of 280
  quads 10 pixels square, each its own i and a j never i, sampling a 32 by 32 texture in 4 by 4 blocks, a block an
  index, every quad's centre checked in each frame. On the console 0 of 16,520 quads were wrong over 59 frames; with
  the rule off (a copy of xsdk_test built without it, its compiler check's header made by that compiler), 16,520 of
  16,520, each quad sampling the block at j.

## a0 and the predicate's execs (2026-10-17)

A pixel shader's if/else on a uniform goes in execs on the predicate where the unroller merges it, in a shader with a
loop or with `gl_FragDepth` under an if/else (`wanted`, `src/xsc_unroll.c`), so the arm not taken is skipped
(xenia:src/xenia/gpu/ucode.h:114-124). xost's new `vk_dynamic_if.frag` reads `colors[i]`, then on a uniform one arm
loads a0 with j and reads `tints[j]` while the other reads `tints[i]` at the i loaded before the arms, then reads
`colors[i]` again. xost put each arm in a `cond_exec_pred` exec, each instruction in it predicated the same way.

**The earlier hang.** xsdk_test's raster test, with xost at `3f35da3` (the reload below) compiling
`vk_dynamic_if.frag` on the console, drew it as a grid of 280 quads in two draws, the arm reading at j taken over the
top half: 585 of 1120 quads were wrong over four frames, then frame 5's fence didn't land, the GPU hung, and the tests
after it couldn't open it (`results/2026-10-17-a0-predicated/xsdk_test_hung.log`). Every wrong quad it logged but one
held the colour the program gives when the arm's read at a0 writes nothing; that one held the clear colour.

**The probe.** `tests/probe_a0_predicated.c` draws pixel shaders `tools/gen_a0_predicated_probe.py` builds by hand
with xost's encoders, each over 128 by 72 quads of 10 pixels, every quad's centre checked: the arms' condition a
uniform, true, false, and true over the top half then false below in two draws; one set of indices for all quads, and
each quad's own. Each case reads `c[k+a0]` or a plain constant into a register that starts at a marker no read gives,
and exports it, so a pixel names the constant read or shows nothing was written. 26 cases, 240 draws, run once on
2026-10-17 (`results/2026-10-17-a0-predicated/probe_a0_predicated.log`); it didn't hang. Hardware facts, the console's:
- **No a0 in the arms: right.** Plain reads in `cond_exec_pred` arms (case 1), and a read at a0 after the setp with no
  arms (case 2), passed every quad of every draw.
- **a0 doesn't reach into an exec on the predicate.** A read at a0 in a `cond_exec_pred` arm that runs, a0 loaded
  before the arms, wrote nothing in any quad (cases 5, 6, 13, and 21 with the condition false); in about a third of
  them nothing was drawn at all, the clear colour left.
- **A skipped exec on the predicate that loads a0 loses it.** With its condition false, a read after the arms at the
  a0 loaded before them wrote nothing in any quad (cases 7 and 8). Loading a0 again after the arms fixed it, every quad
  (cases 9 and 10).
- **xost's encoding with a load in the arm:** a `cond_exec_pred` arm, each instruction predicated too, loading a0 and
  reading at it: 236 to 384 quads of 9216 a draw, about 3%, wrote nothing in the `(p0)` arm (cases 3, 12, 14 to 17);
  the same arm with the exec alone on the predicate (case 23), or the instructions alone predicated in a plain exec
  (case 22), was right in every quad. On `(!p0)` it was right but for one quad of the split draws (case 4).
- **Plain execs, each instruction predicated:** `vk_dynamic_if.frag`'s arms in that shape (case 20) were right in
  every quad of every draw, the `(!p0)` arm's read at the a0 loaded before the arms among them, with no load in it.
- **The condition each quad's own**, so the predicate differs within a vector of 64 pixels: about 6200 quads of 9216
  wrong with both arms (case 24), and about 2250 with the `(p0)` arm alone, which loads a0 and reads (case 25).
- **`vk_dynamic_if.frag` as xost compiled it,** without the reload (`d8206eb`, case 18) and with it (`3f35da3`, case
  19, and 26 over 15 rounds): wrong as above, the quads matching what the raster test logged.

Compiler rules xost keeps, built and run on the console on 2026-10-17:
- **An arm using a0 goes in plain execs.** An arm that loads a0 or reads a constant at it goes in plain execs, each
  instruction predicated as before, cases 20's and 22's shape (`exec_entry`, `src/xsc_program.c`). An arm that doesn't
  stays in execs on the predicate, byte for byte as before.
- **a0 loaded again past the arms.** After arms where one loads a0 at another index, the next read at the index from
  before them loads it again (`emit_predicated`, `src/xsc_lower_flow.c`; `set_a0`, `src/xsc_lower_block.c`): the
  lowering had put its record of a0 back as it was before the arms, so `vk_dynamic_if.frag` read `colors[j]` past the
  arm reading at j, every index wrong on xost's simulator. In a plain exec a predicated `maxasf` whose predicate fails
  doesn't run (xenia:src/xenia/gpu/shader_interpreter.cc:118-121), so the load again is needed only past the arm that
  ran; it's one instruction, and the probe didn't try a read past arms in plain execs.
- **Uniform conditions only.** Arms go on the predicate only in a pixel shader, outside another arm, on a condition
  `xsdk_xsc_find_varying` doesn't mark (`predicated`, `src/xsc_lower_flow.c`): it marks inputs, `gl_FragCoord` and
  `gl_FrontFacing` among them, locals, samples, derivatives, what a hardware loop carries from any of those, and
  everything read from what it marks, the unroller's selects reading their condition. So every pixel of a vector takes
  the same arm, and the divergent predicate of cases 24 and 25 never comes from xost; an if/else on data becomes
  selects.
- **Over the corpus,** milestone 45's, optimiser off and on, and Dusklight's pack: no other program has an arm using a0,
  only testdata's `vk_loop_uniform_if.frag` and vulkan4xenon's `loop_uniform_if.frag` have execs on the predicate, and
  every program but `vk_dynamic_if.frag` compiles byte for byte as before.
- **Checks:** `tests/test_dynamic_index.py` builds a program with one arm loading a0 and reading at it and one reading
  a plain constant, and wants the first in a plain exec and the second in a `(!p0)` exec; it wants both of
  `vk_dynamic_if.frag`'s arms in plain execs and runs it on `xdis/simulate.py`, which predicates each instruction, for
  16 indices, both arms, optimiser off and on. xsdk_test's raster test draws it as `3f35da3` had it.
- **Xenia doesn't show it:** its a0 changes only at `maxa`, `maxas` and `maxasf`, whatever exec they're in
  (xenia:src/xenia/gpu/shader_interpreter.cc:600-602, 808-823), and so does xost's simulator.

Still unknown:
- **The hang's cause.** The probe drew the program that hung in 96 draws without hanging; the raster test drew it
  among the other grids of its frames, which the probe doesn't.
- **A divergent predicate.** Cases 24 and 25 fail in xost's old encoding, which also mixes in a0's own failures; a
  divergent predicate in plain execs, or with no a0 at all, wasn't tried.
- **a0 past a skipped exec on the predicate that doesn't use it.** Case 7 lost a0 past an arm that loads it; whether
  an exec on the predicate that doesn't touch a0 loses it too wasn't tried. No program of the corpus reads at a0 past
  such an exec.

On the console on 2026-10-17, xsdk_test's raster test drew xost's `vk_dynamic_if.frag`, compiled there with both rules,
over its two grids, the arm taken in the first: 0 of 16,520 quads wrong over 59 frames, in each of two runs, and no
hang. With the load again past the arms off, 8,260 of 16,520, the arm-taken half. With the plain-exec rule off the
program is commit `3f35da3`'s, the run in `results/2026-10-17-a0-predicated/xsdk_test_hung.log`: half the arm
quads wrong, then the GPU hung at frame 5; it wasn't run again.

## xost's code quality, milestone 45 (2026-10-17)

docs/scope/45-xost-code-quality.md. `tests/probe_shader_cost.c` measured each step on the console, run through the
console agent (logs: `results/2026-10-17-milestone-45/`, whose README also has the PC's counts and each step's checks).
- **What it times.** Each of the 150 programs in Dusklight's pack, compiled on the console from the pack's SPIR-V with
  the optimiser on, as vulkan4xenon compiles them. 4 draws of each are timed by the GPU's clock, copied with the GPU
  idle before and after, at 498.75 clocks a us, and the best of 3 rounds counts. Each line logs the program's ALU
  instructions and registers, which equal `tools/check_optimize.py`'s counts for it.
- **Pixel shaders** each draw two triangles over a 1280x720 target, 921,600 pixels, behind a vertex shader of the
  probe's own. Every constant holds a fixed sixteenth, the shader's literals over them, and every sampler a 64x64
  texture, filtered, one level. Each picture is resolved and hashed, so a step that changes what a pixel shader
  computes shows.
- **Vertex shaders** each draw 65,535 vertices whose inputs all hold the same four values, so every triangle has no
  area. A visibility query around one draw shows nothing reaches the pixels.

What it leaves out:
- **3 pictures that never repeat.** `1510c05add37f2b3.frag`, `29fab4bb2516d607.frag` and `2eefbbd89e53921c.frag` hash
  differently in every run, the baseline's two as well, so each step's pictures are compared on the other 100. All
  three sample at coordinates offset by another texture's texel times 255, GX's indirect texturing, so the coordinates
  run into the thousands. Two other shaders that do the same repeat; why these three don't was found after the
  milestone ("Slow fetches across execs", below).
- **Interpolators a pixel shader doesn't read.** Each pixel shader is drawn with one past its highest input Location.
  The probe's first run drew all 16 for every shader, and every pixel shader up to 45 ALU instructions then took about
  1,852 us, a pixel a clock: interpolating 64 components a pixel set the pace, and hid what their code cost.
- **Vertex shaders' results.** With no area, their exports reach no pixel, so the probe times them and checks nothing
  they compute. The PC's simulator compares their exports with the optimiser on and off (`xdis/equivalence.py`), and
  Dusklight's title scene drew them at every step.

Each shader's time summed, against the baseline as each shader's better time in its two runs
(`probe_baseline_1.log` and `_2.log`, 0.46% apart a shader on average):

| Run | Pixel shaders, us | Vertex shaders, us | Registers a pixel and a vertex shader | ALU, pixel and vertex |
|---|---|---|---|---|
| Baseline | 77,668 | 16,472 | 6.19, 9.77 | 1687, 4930 |
| Step 2, values computed into place (`probe_step2.log`) | 66,036 (-15.0%) | 14,054 (-14.7%) | 6.21, 7.49 | 1342, 3593 |
| Step 3, registers packed (`probe_step3.log`) | 62,830 (-19.1%) | 14,056 (-14.7%) | 4.53, 6.34 | 1342, 3593 |
| Step 4, more co-issue (`probe_step4.log`) | 62,921 (-19.0%) | 13,916 (-15.5%) | 4.53, 6.38 | 1342, 3546 |
| The a0 reload (`probe_reload.log`) | 62,830 (-19.1%) | 13,935 (-15.4%) | 4.53, 6.38 | 1342, 3548 |
| Step 6, unused fetches dropped (`probe_step6.log`) | 62,894 (-19.0%) | 13,923 (-15.5%) | 4.52, 6.38 | 1342, 3548 |
| Steps 5 and 6, fetch constants (`probe_step56.log`) | 62,947 (-19.0%) | 13,458 (-18.3%) | 4.52, 6.38 | 1342, 3276 |
| Step 2's copies together (`probe_together.log`) | 62,913 (-19.0%) | 13,349 (-19.0%) | 4.52, 6.38 | 1342, 3257 |

- **The gain.** Dusklight's pixel shaders take 19.0% less of the GPU and its vertex shaders 19.0%, 19.0% together. 93
  of the 150 programs take more than 2% less than their better baseline time, and none more than 2% above their worse.
  Every run drew the same 100 pictures as the baseline, and no vertex draw reached the pixels.
- **Where it came from.** The pixel shaders' from step 2, and step 3's registers, 4.9% less than at step 2 with 4.53
  registers a shader for 6.21. The vertex shaders' from step 2; step 4, in the two vertex shaders read at a run-time
  index, `93d36ce55e3f9bad.vert` 638.7 to 578.4 us and `c0ddfd4547c71e5a.vert` 704.4 to 635.9; and step 5, 3.3% less
  than at step 6; and step 2's copies placed together, 0.8% less again. Their ALU falls by 33.9%, their time by 19.0%.
  The baseline's two runs differ by 0.2% in the pixel
  shaders' total, so the pixel shaders' moves after step 3 are noise.
- **Registers.** Pixel shaders take 4.52 a shader for 6.19 and vertex shaders 6.38 for 9.77. The two vertex shaders read
  at a run-time index take one more each than before the milestone, from step 4: `93d36ce55e3f9bad.vert` 14 and
  `c0ddfd4547c71e5a.vert` 13.
- **Compiling the pack on the console,** as `probe_xsc` times its 150 programs from a pack built by each step's xost:
  1.32 s before the milestone, 3.39 s at step 2, 1.42 s once step 2's pass stopped the optimiser's round starting over
  for every copy it took (`probe_xsc_compile_time.log`), 1.45 s at step 4, 1.43 s with the a0 reload and at step 6,
  1.32 s at steps 5 and 6, and 1.33 s with step 2's copies placed together (`probe_xsc_together.log`). Every program
  came out on the console as on the PC.
- **The PC's counts,** `tools/check_optimize.py` over the 367 shaders the baseline compiled, optimiser on
  (`baseline.json` against `step2_together.json`): ALU 8895 to 6467 (-27.3%), copies 3253 to 903 (-72.2%), co-issued
  slots 675 to 726, registers 2115 to 1529 (-27.7%), control flow 2573 to 2173 (-15.5%), fetches 614 to 610.
  Dusklight's and vkbench's 157: ALU 6641 to 4620 (-30.4%), copies 2397 to 431, co-issued slots 583 to 630, registers
  1117 to 784, control flow 1581 to 1239.

The milestone also found that a0 doesn't survive an alloc ("a0 doesn't survive an alloc", above), which had lit
Dusklight's skinned models from the wrong rows before it and at its step 4.

## Slow fetches across execs (2026-10-17)

"Serialized fetches" (2026-10-15) took an exec boundary for a wait, as every program that drew right read a fetch's
result in a later exec. It isn't one: a slow fetch's result read in the next exec came too soon, and only the
serialize bit makes an instruction wait. Logs: `results/2026-10-17-fetch-wait/`.
- **What showed it.** `1510c05add37f2b3.frag`, `29fab4bb2516d607.frag` and `2eefbbd89e53921c.frag`, the three of
  Dusklight's pixel shaders that drew a different picture in every run of `tests/probe_shader_cost.c` (milestone 45,
  "3 pictures that never repeat"). Each does GX's indirect texturing, a texel times 255 offsetting another fetch's
  coordinates into the thousands, and each reads its first fetch's result in the exec after it, unserialized. The two
  indirect-texturing shaders that repeated, `820f98f280587ea8.frag` and `ca44599827bbdb23.frag`, read it in the fetch's
  own exec, which the 2026-10-15 rule serialized.
- **The experiment.** xost with what fetches wrote tracked for the whole program, not reset at each exec, and cleared
  only by a serialized instruction. 140 of the pack's 150 programs changed, each only in its serialize bits. In two
  runs of `probe_shader_cost` (`probe_shader_cost_1.log`, `probe_shader_cost_2.log`) the three drew the same picture
  both times, `BE5FB0A3`, `5FB6B0A3` and `891EC12B`, and the other 100 drew the pictures milestone 45 ended with
  (`results/2026-10-17-milestone-45/probe_together.log`).
- **What it costs.** A draw of each pixel shader took 65,487 and 65,498 us against milestone 45's 62,913, 4.1% more;
  each vertex shader 13,355 and 13,386 us against 13,349, within the 0.46% a shader moves between runs. 19 pixel
  shaders take more than 5 us longer, most of all `b4a8c01218dcffe9.frag` (+38%), `b40238367bc2ae70.frag` (+35%) and
  `6dd3a70373df59bf.frag` (+31%): in each the fog table's fetch, at coordinates the ALU works out in the exec before,
  is serialized after three fetches that need nothing, so it waits for them too.
- **Won back by putting such a fetch first.** Fetch grouping now puts a fetch that needs the ALU's work ahead of
  those that don't in its run (`take_ready_round`, `src/xsc_program.c`), so it waits for the work alone. 21 of the
  pack's pixel shaders change, only in their fetches' order and serialize bits. In two runs of `probe_shader_cost`
  (`probe_shader_cost_ordered_1.log`, `_2.log`) a draw of each pixel shader took 62,917 and 63,045 us against
  milestone 45's 62,913, within run to run; each vertex shader 13,361 and 13,348. All 103 pictures were the
  experiment's.
- **xost now** (`exec_entry` and `control_flow`, `src/xsc_program.c`) serializes an ALU instruction that reads or
  writes a temp a fetch wrote since the last serialized instruction, in any exec, and a fetch that reads a temp any
  instruction wrote. A loop's body starts with what any instruction in it writes marked, as a pass reads what the pass
  before wrote, and what was marked before a loop stays marked past it, as a loop may run no times. Code on the
  predicate, an exec on it or one whose instructions are, may not run, so what was marked before it stays marked after
  it. Dusklight's pack compiles byte for byte as in the experiment: 93 of its pixel shaders and all 47 vertex shaders
  change, serialized ALU instructions 4 to 100 and 0 to 47, serialized fetches 5 to 36 and 0 to 0.
- **Vertex shaders keep the rule.** Every one so far drew right with its vertex fetches read unserialized in a later
  exec, but a vertex fetch reads memory as a texture fetch does and can miss a cache too, and the first instruction
  reading one waiting cost nothing measurable. The vertex cache mask ("A fifth vertex fetch") depends only on how many
  vertex fetches an exec holds, which this doesn't change.
- **Checks.** `tests/test_dependent_fetch.py` runs the rule over each program's control flow, loops and code on the
  predicate as paths, and wants no instruction it needs left unserialized, over xost's testdata and hand-built
  programs: a fetch's result read in a later exec, a serialized instruction in between, a loop, arms on the predicate.
  With what fetches wrote reset at each exec, with a loop's writes left unmarked, with what was marked before a loop
  dropped past it, or with an arm's serialized instruction taken as a wait for the other arm, it fails. xsdk_test's
  raster test compiles xost's `vk_slow_fetch.frag` on the console and draws it over 128 quads 10 pixels square, every
  pixel of every frame checked: two fetches far across a 1024 by 1024 texture, each pixel's 37 columns and 48 rows
  from its neighbours', so in a tile of its own, read in the next exec by the work that picks the row a third fetch
  reads. Each of the texture's rows holds row mod 16's colour, so a pixel's colour is its quad's whatever column it
  samples. On the console (`xsdk_test.log`) it had 0 of 755,200 pixels wrong; built with what fetches wrote reset at
  each exec (`xsdk_test_nofix.log`), 710,754 wrong. vulkan4xenon_test passed 426 of 426 twice and vkbench 14 of 14,
  their shaders built with `--optimize`, and Dusklight's title scene ran as at milestone 45's end, 30 fps.
- **Not known.** In milestone 45's binaries of the pack, the three were the only programs with an ALU instruction
  reading a fetch's result, unserialized, with no alloc between: every such read in the other 137 programs came past
  one, and xost's allocs have Xenia's `is_unserialized` clear (xenia:src/xenia/gpu/ucode.h:488). Whether a serialized
  alloc waits for the fetches before it wasn't tried. Nor was whether a fetch must wait for the ALU's results from an
  earlier exec: 28 pixel shaders read them unserialized, and 25 of those drew the same pictures every run.

## Tiling's registers and packets, milestone 44 step 1 (2026-10-17)

`tests/probe_tiles.c` on the console, its cases 1 to 4 twice and 5 to 10 each alone from a fresh boot
(docs/scope/44-tiling-and-msaa.md, step 1). Logs: `results/2026-10-17-milestone-44-step-1/`.
- **A window offset moves a band to eDRAM's top.** `PA_SC_WINDOW_OFFSET` at (0, -64) put a 256x128 target's rows 64
  to 127 at eDRAM rows 0 to 63, under a 256x64 `PA_SC_SCREEN_SCISSOR`, every pixel and nothing else
  (`probe_tiles.log`, `probe_tiles_2.log`, case 1). The window scissor moved with the offset, and with
  `window_offset_disable` stayed where it was set, as Xenia has both (xenia:src/xenia/gpu/registers.h:586-633). The
  offset flipped to (0, 64) drew nothing where it belonged (`probe_tiles_flip.log`).
- **`gl_FragCoord` gives the row in the band.** Under the offset, the pixel parameters xost reads with
  `SC_OUTPUT_SCREEN_XY` held each pixel's eDRAM row, 0 to 63, not its row in the target, every one of 16,384 (case 2),
  as Xenia reads them (xenia:src/xenia/gpu/dxbc_shader_translator.cc:705-725). A tiled pass must add the band's top.
- **A resolve lands at a band's rows with `RB_COPY_DEST_BASE` moved.** 32-row bands resolved with the base moved by
  the row times the pitch times 4 bytes landed at rows 32, 64 and 96 of a tiled texture, the rows around them left
  alone (case 3), as Xenia addresses a tiled texture in 32-row steps (xenia:src/xenia/gpu/texture_address.h:190-207).
  Not moved, every band landed at row 0 (`probe_tiles_nomove.log`).
- **Depth stays in eDRAM across bands with `RB_DEPTH_INFO`'s base moved.** Four 32-row bands drew a fill and a near
  quad, then a far quad hidden behind the near one, each band's depth at its own base, one wrapping from tile 2044
  past 2047 to 3: every pixel right in both passes (case 4).
- **The hierarchical buffer doesn't follow the depth base; bits 18 to 26 of `RB_HIZCONTROL` move it, in 512
  pixels.** With the bits left at 0, every band's drawn quads were rejected against band 0's near depth, 24,576 pixels
  wrong. Moved by the band's 32 rows of a 320-pixel pitch over 512, 20 a band, every pixel was right. Moved by the
  band's 8 eDRAM tiles, band 1 overlapped band 0's and lost 1,024 pixels. Hidden quads took 3 us for 8 full-band quads
  of the long shader against 185 us drawn, so the buffer rejected them. Milestone 30 saw the same unit from the other
  side ("Hierarchical depth, milestone 30": 512 pixels for bit 18).
- **Predication works as Xenia models it, with `PM4_SET_BIN_MASK` and `PM4_SET_BIN_SELECT`.** A type-3 packet with
  header bit 0 ran only where select and mask shared a bit, over all 64 bits: masks 1, 2, 1 << 32 and all ones under
  selects 1, 2 and 1 << 32 landed and skipped as xenia:src/xenia/gpu/command_processor.cc:704-713 and 815-828 say, in
  the ring (case 5) and in a recording run with `PM4_INDIRECT_BUFFER` (case 8). Without the bit every draw landed
  (`probe_tiles_nopred.log`). Each register's two words sent the other way round gave the same (case 6), so which word
  is high isn't known: the probe sends both registers alike.
- **The one-word packets wedge the GPU.** `PM4_SET_BIN_MASK_LO` (0x60) and the three after it, sent alone, never let
  the fence land: 0x1740 read 0x80050110, busy, and 0x07D8 0x20007FFF, bit 29 set beside the 0x7FFF it holds at title
  start, bit 28 (halted) clear; the read pointer had reached the write pointer (case 7). Xenia handles them
  (command_processor.cc:793-814); the console doesn't. Tiling uses 0x50 and 0x51.
- **XAM leaves predicated draws running.** A predicated draw before any bin packet landed (case 9), on a fresh boot and
  after a title that left both registers all ones and ended (case 10). Whether XAM's own drawing, the Guide over the
  dashboard, is right after that wasn't looked at.

## Listing a folder on a FAT32 USB stick (2026-10-17)

github.com/xenosworks/xsdk issue 1: on an RGH console, listing a folder that has entries on a FAT32 stick
(`\Device\Mass0`) crashed the title about a second later; the hard drive was fine. `tests/probe_usb_dir.c` on the
console with a FAT32 stick in, logs in `results/2026-10-17-usb-dir/`:
- **The stick's driver leaves `next_entry_offset` and `file_index` unwritten.** Each `NtQueryDirectoryFile` into a
  buffer of 0xA5 returned one entry, even with 20 KB of room, and left both fields 0xA5A5A5A5; it wrote the times,
  sizes, attributes, name length and name (67 bytes for `sub`, 69 for `a.txt`), then answered 0x80000006 when no
  more were left.
- **`readdir` followed what the buffer held.** It took a non-zero `next_entry_offset` for another entry in the same
  buffer, so a `DIR` from heap memory holding old bytes read far past it: from a heap whose free blocks held the
  pattern, it listed `sub` and stopped, and the title died between 0.5 and 1 s later, as the issue says
  (`probe_usb_dir_nofix.log`). From fresh, zeroed heap it had listed right, which is why it went unseen.
- **xsdk now clears the buffer before each query** (src/dir.c), so a field the driver leaves reads 0, the last entry.
  From the same dirty heap both folders listed right and the title lived on (`probe_usb_dir.log`); xsdk_test's
  filesystem checks, on the hard drive, passed (`xsdk_test.log`).

## Tiled passes at one sample, milestone 44 step 2 (2026-10-17)

xsdk's tiled passes (`xsdk_gpu_begin_tiles`, `src/gpu_tiles.c`) on the console: a pass recorded once and run a band of
rows at a time, as step 1 found the registers and packets work. Logs: `results/2026-10-17-milestone-44-step-2/`.
- **Every pixel as untiled.** xsdk_test's tiles checks (`xsdk_test.log`) passed: 1280x720 in 2 bands of 384 rows and
  3 of 256, with triangles across the band edges, xost's `gl_FragCoord` gradient, hidden quads over hierarchical
  depth and a draw given band 0's rows that reaches into band 1; the same with depth whole; four targets, 8_8_8_8,
  2_10_10_10, 16_16_16_16_FLOAT and 32_FLOAT, with depth, 4320 tiles in 3 bands; a 2048x2048 depth-only pass in 2
  bands, its depth resolved a band at a time and then sampled. Each was compared with the same draws untiled.
- **`gl_FragCoord` reads c255.** xost adds the pixel shader's constant 255 to the pixel parameters, and xsdk keeps it
  at 0.5, and 0.5 more than the band's top in a tiled pass (`XSDK_XSC_FRAG_COORD_CONSTANT`). A shader reading it
  leaves c252 to c255 to xsdk.
- **Each break failed what it aims at** (`xsdk_tiles_*.log`, built from `local/m44/step2_breaks.patch`): the window
  offset left at 0, or the resolve's base not moved, failed the 1280x720 frames, the four targets and both 2048x2048
  checks; a band's resolve not clearing failed all those but depth whole; the bin select left all ones, or
  `gl_FragCoord`'s constant left at band 0's, the three 1280x720 frames; the hierarchical buffer left uncleared, the
  two band frames and the four targets.
- **What a band costs** (`probe_tiles.log`, case 11, by the GPU's clock at 1280x720): a band's fixed cost 5 us; a
  fill-bound frame 10,331 us untiled and 10,349 in 3 bands; a vertex-bound one 2,403, 4,515 and 6,622 us in 1, 2 and 3
  bands, each band running every draw's vertices again; a predicated draw outside its band 0 us against 2,103 drawn.
- **Unchanged elsewhere.** vulkan4xenon_test passed 426 of 426 twice and vkbench 14 of 14, built against it; Dusklight,
  its pack rebuilt with this xost, ran its title scene at the frame rates before.
- **Depth whole keeps hierarchical depth off.** Step 1 found bits 18 to 26 of `RB_HIZCONTROL` move the buffer, but a
  recording writes the register with every depth state, so it can't differ a band; and bands keep it on only up to
  1024 tiles of depth, the 1280x1024 milestone 30 checked.

## Render passes past eDRAM in the layer, milestone 44 (2026-10-18)

vulkan4xenon draws a subpass whose attachments pass eDRAM's 2048 tiles as xsdk's tiled pass at one sample: its
commands go once into a recording each band runs, a load drawn back at the recording's start, a store a band's resolve,
depth kept whole at eDRAM's top when it fits and otherwise stored a band at a time and drawn back from its texture.
Logs: vulkan4xenon:results/2026-10-18-milestone-17-part-1/.
- **The hierarchical buffer rejected pixels it shouldn't at a 2048 pitch.** A 2048x2048 shadow map with a colour
  attachment ran in 4 bands of 512 rows, each band's depth 832 tiles, under the 1024 xsdk then allowed the buffer: from
  row 812 on, 300 rows into the second band where the first band's near square had stood, 1,652,496 of 4,194,304
  texels kept the clear's depth (`vulkan4xenon_test_hiz_on.log`). With the buffer off past the 1280x1024 milestone 30
  checked ("Hierarchical depth, milestone 30"), which xsdk now keeps to, tiled or not, every texel was right. Why a
  wider pitch goes wrong isn't known.
- **xsdk keeps pixel constants c252 to c255.** `xsdk_gpu_set_pixel_constants` and `xsdk_gpu_load_pixel_constants`
  assert below `XSDK_PIXEL_CONSTANTS`, so nothing moves `gl_FragCoord`'s constant (c255, "Tiled passes at one
  sample"); the layer refuses a fragment shader whose constants reach c252 and gives `maxUniformBufferRange` as 4032.
- **Checks.** vulkan4xenon_test passed 439 of 439 twice: four attachments of four formats at 1280x720 stored by one
  tiled pass and loaded by the next, an untiled pass loading the depth they left at eDRAM's top, a 2048x2048 shadow
  map stored a band at a time, loaded back and sampled, every texel checked, and the refusals. Each of five break sets
  failed the checks it names. xsdk_test, vkbench, probe_tiles, probe_shader_cost, probe_a0_predicated and Dusklight
  ran as before.

## Loops on the console, milestone 46 step 1 (2026-10-18)

`tests/probe_loop_break.c` on the console, its pixel cases in order and each vertex case alone from a fresh boot
(docs/scope/46-loops.md, step 1). Logs: `results/2026-10-18-milestone-46-step-1/`. Every shader ran a hardware loop
whose LOOP_END broke on the predicate; no exec ran on it. Over 128 by 72 quads of 10 pixels, each pixel left at pass
B = px + 10 py + base, its place in its quad and the quad's own base, 1 to 200, so the pixels of a group disagreed.
- **A group of pixels loops until every pixel in it has left.** An ungated pass counter held each pixel's own B in
  14,864 of 921,600 pixels and more in the rest, never fewer (case 1), as Xenia's comment on the break has it, "if the
  predicate in all 64 invocations matches" (xenia:src/xenia/gpu/ucode.h:372-373), though its translator breaks each
  pixel alone (xenia:src/xenia/gpu/spirv_shader_translator.cc:986-990).
- **A group holds the 2x2 pixel quads of about one triangle,** not a tile of the screen: the first quad's top-left
  triangle all held 92, the largest B among the pixel quads it touches, and its other triangle 100 (the counts over
  pixels 0 to 39 in `probe_loop_break.log`). 86% of aligned 2x2 tiles held one count, 25% of 4x4 and no 8x8.
- **Gating holds each pixel's own values.** The scope's design, i and w kept by `cndgt` on 1.0 or 0.0 flags and the
  break on no pixel alive, left i at B and w at B + 49 in every pixel, with the groups held back as in case 1 (case
  2a), and with every pass of 255 run (case 2b).
- **Leaving early costs nothing past the passes run.** A full-screen loop of count 255 that every pixel left at pass 1
  took 593.2 us, the same as a loop of count 1 at 592.9 us (cases 3a and 3b). 128 passes took 20.7 ms and 255 took
  40.8 ms, 158 us a pass (3c, 3d). With half the pixels of every quad leaving at pass 1 and the rest never, it took
  40.8 ms, as if none left (3e). Thin triangles of about 8 pixels, 74,000 pixels in all, took 85 us leaving at pass 1
  and 4.1 ms running all 255 (3f, 3g), against 3.2 ms for as many pixels in full groups at 3d's rate.
- **Loops nest 4 deep,** each level's count from its own loop constant, and a break in the inner of two loops left only
  the inner (cases 4a to 4d).
- **a0 survives LOOP_START and LOOP_END:** loaded before the loop and read in each pass, loaded in the pass and read
  after it, and both in the pass, every pixel read the row it loaded (cases 5a to 5d). Nothing allocated between.
- **Vertex shaders' loops ran and didn't hang,** each from a fresh boot: a break all vertices agree on (6a), the gated
  design with a break each quad's vertices took at their own pass (6b), most of them run past it as pixels are, and
  loops nested 2 and 4 deep (6c, 6d).
- **A sample at the computed LOD in each of 4 passes took each pass's level** (case 7b).
- **A wait between a setter and its fetch lets another pixel group's state in.** The LOD `setTexLOD` sets and the
  gradients' setters' are kept per lane and shared between the pixel groups in flight. Each case sets a level that
  differs between groups, a quad's base mod 4 or a pass's index:
  - With the setter serialized and the fetch after it not, as xost emits them, every pixel took its level: one sample
    (7f), the two alone in an exec after the one working the LOD out (7g), `setGradientH` serialized and
    `setGradientV` and the fetch not (7i), and a sample in each of a loop's 4 passes (7j).
  - With the fetch serialized as well, a level per quad was right in 21% of pixels, about chance among 4 (7c). A level
    per pixel, its column mod 4, was right in 59%, and each of the 4 wrong pixels logged held the level of a pixel in
    the same place in another quad, 2 for 0, 3 for 1, 0 for 2 and 1 for 3, never a neighbour's in its own (7d). In a
    loop, 4% took levels 0 to 3 and the rest 1, 2, 3 and 3, each pass the level of the pass after it, on a base-4
    count of the levels taken (7e, 7a). `setGradientV` serialized after `setGradientH` was right in 50% (7h).
  - xost emitted the sequence that holds only as its inputs happened to fall: a setter whose input was written before
    the last wait went out unserialized ahead of a fetch that waited, and fetch grouping could take a setter a round
    ahead of the work its fetch read, as `xenos-toolchain/xsc/testdata/vk_lod_dependent.frag` showed. A run of setters
    now goes only with its fetch, once all of it is ready, and waits at its first setter if any of it would
    (src/xsc_program.c, `run_end` and `run_waits`); `xenos-toolchain/tests/test_dependent_fetch.py` checks every
    run in the testdata, and fails with the library before. None of the 792 programs of milestone 45's corpus
    compiles differently, and xsdk_test passed 409 of 411 with it, the two controller checks failing with no pad in.
    Milestone 22's bands, each one LOD, couldn't show any of it.
- **Break builds.** Gating removed failed 2a and 2b; the break's condition inverted failed case 1 and 2a; the nested
  loops' constants swapped failed 4a to 4d; the sample taken once before the loop failed 7b and 7j; a0 never loaded
  failed 5a.

## Breaks and continues on data in unrolled loops, milestone 46 step 2 (2026-10-18)

xost gates a break or continue on data in an unrolled loop as docs/scope/46-loops.md designs it, with stopped copies
selected by `cnd` on 1.0 and 0.0 flags (src/xsc_unroll.c, `stop_pass`), the shape step 1's case 2 proved. Logs:
`results/2026-10-18-milestone-46-step-2/`.
- **Every pixel left at its own pass.** xsdk_test's raster grid, `vk_loop_grid.frag` compiled on the console, 80 cells
  of 10 by 10 pixels, each pixel's place picking a break on data in arms of an if/else, a continue on data and a break
  the unroller folds: 0 of 472,000 pixels wrong over 59 frames. The console's compiles of it, `vk_loop_if_break.frag`
  and `vk_vertex_loop_break.vert` matched the PC's.
- **Break builds:** the folded break ignored got all 472,000 wrong, and a continue lowered as a break 202,960.
- **Nothing else moved:** no other program of milestone 45's corpus compiles differently, Dusklight's pack among them;
  vulkan4xenon_test passed 439 of 439 twice, vkbench 14 of 14, and Dusklight's title scene ran at its frame rates
  before.

## Breaks and continues on data in hardware loops, milestone 46 step 3 (2026-10-18)

xost gates a break or continue on data in a hardware loop as step 2 gates one in an unrolled loop, with `alive` a flag
the loop carries from 1: a `setp_eq` on it at the pass's end sets p0 where the pixel has left, and LOOP_END breaks on
p0, so a group leaves once none of its pixels is left (src/xsc_unroll.c, `carry_alive`; src/xsc_lower_flow.c,
`emit_loop`). The counter adds `alive`, so after the loop it holds the pass a pixel broke at. Logs:
`results/2026-10-18-milestone-46-step-3/`.
- **Every pixel left at its own pass.** xsdk_test's raster grid, `vk_loop_count_grid.frag` compiled on the console, one
  quad of 112 cells of 10 by 10 pixels, the pixels of each 2x2 quad and each group of 64 leaving at different passes on
  a continue on data, a break on data in an arm of an if/else and one ending the pass, the counter after the loop added
  to red: 0 of 660,800 pixels wrong over 59 frames. The console's compiles of it, `vk_loop_break_data.frag`,
  `vk_loop_count_stop.frag` and `vk_vertex_loop_count_break.vert` matched the PC's.
- **Leaving early costs nothing.** The same shader over the whole 1280x720 target, every pixel leaving at the first
  pass, the best of 4 draws each: 1,444,291 GPU clocks under a count of 255 against 1,444,310 under a count of 1, about
  2.9 ms at 498.75 MHz.
- **Break builds:**
  - gating off in the hardware loop: 554,718 of the grid's 660,800 pixels wrong, the early exit timed as before;
  - the predicated LOOP_END dropped: every pixel right, and 226,244,322 clocks under a count of 255, about 454 ms, 157
    times a count of 1.

  In both, the console's compiles of the four gated shaders differed from the PC's.
- **Nothing else moved:** of milestone 45's corpus, only `vk_loop_break_data.frag`, refused before, and step 3's new
  testdata compile differently; Dusklight's pack is the same byte for byte. vulkan4xenon_test passed 439 of 439 twice,
  vkbench 14 of 14, and Dusklight's title scene ran at its frame rates before.

## Loops whose count is data, milestone 46 step 4 (2026-10-18)

xost runs a loop whose count is data as a hardware loop of 255 passes, the most a loop constant counts
(xenia:src/xenia/gpu/xenos.h:1086-1097), its condition a gated break at the top of the pass, or at the end for a
`do`/`while`, so a group leaves once every pixel's condition has (src/xsc_unroll.c, `expand_data_loop`,
`top_of_pass`, `end_of_pass`). The runtime sets that loop constant from the new `XSDK_XSC_FIXED_LOOP` need; the layer
carries it as `V4X_RESOURCE_FIXED_LOOP`. Logs: `results/2026-10-18-milestone-46-step-4/`.
- **Every pixel ran its own counts.** xsdk_test's raster grid, `vk_loop_data_grid.frag` compiled on the console, one
  quad of 128 cells of 10 by 10 pixels, each pixel's place giving its counts, 0 to 321, to a `for` loop on a float, a
  `while` left on data too and a `do`/`while` continued past on data, each loop's counter after it a colour: 0 of
  755,200 pixels wrong over 59 frames, the counts past 255 stopping at 255. The console's compiles of it,
  `vk_loop_float_count.frag`, `vk_loop_data_count.frag`, `vk_loop_while.frag`, `vk_loop_count_skip.frag` and
  `vk_vertex_loop_data.vert` matched the PC's.
- **The layer:** vulkan4xenon_test's new scene, `loop_data.frag` from the bundle and compiled on the console, each
  eighth of a strip its own count from `gl_FragCoord`, 15 to 295: every frame and the image on screen right, 255 and
  295 both run as 255. vulkan4xenon_test passed 447 of 447 twice.
- **Break builds:** xsdk_test writing the fixed loop constant as 0 got all 755,200 pixels wrong; the layer writing it
  as 0 failed the scene's four colour checks and nothing else.
- **Nothing else moved:** of milestone 45's corpus, only step 4's new shaders compile differently; Dusklight's pack is
  the same byte for byte. vkbench passed 14 of 14, and Dusklight's title scene ran at its frame rates before.

## Nested loops, milestone 46 step 5 (2026-10-18)

xost nests hardware loops 4 deep, as step 1 found the console does, and keeps one inside an if/else on data or an
unrolled loop: each level its own gating, an inner loop's `alive` starting from the pass it's in, the builder a stack
of open loops (src/xsc_unroll.c, src/xsc_lower_flow.c, src/xsc_program.c). Logs:
`results/2026-10-18-milestone-46-step-5/`.
- **Every pixel took its own passes at each level.** xsdk_test's raster grid, `vk_loop_nest_grid.frag` compiled on the
  console, one quad of 192 cells of 10 by 10 pixels: a loop a push constant counts, continued past on data, around a
  `while` on data, left on data, holding an if/else on data, a checkerboard, with a loop the other push constant
  counts, left on data, in one arm and a `do`/`while` on data in the other: 0 of 1,132,800 pixels wrong over 59 frames.
  The console's compiles of it, `vk_loop_nest2.frag`, `vk_loop_nest4.frag`, `vk_loop_in_data_if.frag`,
  `vk_loop_in_uniform_if.frag`, `vk_loop_in_unrolled.frag` and `vk_vertex_loop_nest.vert` matched the PC's.
- **Nested loops can share a loop constant.** The grid's `while` and the `do`/`while` inside it both run from loop
  constant 17, at 255 passes; each level kept its own count of passes.
- **Break build:** the outer and inner loops' constants swapped got 996,864 of the 1,132,800 pixels wrong, the count
  xost's simulator gives, and nothing else.
- **Nothing else moved:** of milestone 45's corpus, only step 5's new shaders compile differently; Dusklight's pack is
  the same byte for byte. vulkan4xenon_test passed 447 of 447 twice, vkbench 14 of 14, and Dusklight's title scene
  ran at its frame rates before.

## A block loop's passes from its start and step, milestone 46 (2026-10-18)

A loop a block's n counts, from a start by a step, runs ceil((n - start) / step) passes, 0 to 255, as Shader Model 3's
`loop` takes its count from an integer constant the CPU sets: xost's need carries the start and step, and xsdk_test
and the layer write `xsdk_xsc_loop_passes` into the loop constant (xenia:src/xenia/gpu/xenos.h:1086-1097). This
replaces step 4's skipped passes, which ran n passes and stopped at 255 of n rather than of the loop's own. Logs:
`results/2026-10-18-milestone-46-pass-counts/`.
- **xsdk_test:** `vk_loop_step_grid.frag` compiled on the console, a loop from 100 and one by 2 to a push constant's
  300, each its passes and counter after as colours: 200 and 150 passes, 0 of 283,200 pixels wrong over 59 frames.
  The console's compiles of it and of `vk_loop_count_skip.frag`, from -3 and by 2, matched the PC's.
- **The layer:** vulkan4xenon_test's new scene, quads at counts 0 to 600 from the bundle and compiled on the console,
  each loop's passes as worked out: 400 by 2 ran 200 passes and 600 ran 255. vulkan4xenon_test passed 455 of 455
  twice.
- **Break builds:** xsdk_test writing the plain n got all 283,200 pixels wrong; the layer writing it failed the new
  scene's four colour checks and nothing else.
- **Nothing else moved:** every loop from 0 by 1 compiles as before, and Dusklight's pack is the same but for the two
  fields each resource gained. vkbench passed 14 of 14, and Dusklight's title scene ran at its frame rates before.

## Samples in a loop's pass, milestone 46 step 6 (2026-10-18)

xost lowers a sample or derivative of what a hardware loop's pass works out inside the pass, after its inputs, its
fetches serialized and grouped there as elsewhere, a setter run whole (src/xsc_lower.c, `mark_pass_work`). Logs:
`results/2026-10-18-milestone-46-step-6/`.
- **A LOD from register gradients is one per 2x2 quad.** A fetch at the gradients `setGradientH` and `setGradientV` set
  takes its LOD from its quad's first pixel inside the triangle drawn, top left, top right, bottom left then bottom
  right, not each pixel's own; a LOD `setTexLOD` sets is each pixel's own. The first grid,
  `vk_loop_sample_grid_first.frag`, sampled at gradients that differed within a quad, and got 148,680 of 283,200
  pixels wrong, the same in every frame of two runs. Its model with this rule and the next, `first_grid_model.py`, gives
  every pixel of a frame as the console drew it, 0 of 4,800 unlike; without either, 1,200 or 1,888. In a gated loop a
  pixel that left holds its counter, so gradients worked out from it go on steering its quad's LOD.
  - xost keeps the hardware's rule. Gradients the same across a quad take the same LOD either way, as derivatives of
    a value linear across the quad are, parallax occlusion's among them; a LOD per pixel would mean working it out in
    the ALU from the texture's size through `setTexLOD`, losing anisotropic filtering. Whether the console's
    derivatives are a quad's or a row's is still unknown: the grid's are of linear values, alike either way.
- **rcp lands a hair under a third and a fifth.** `mod(x, 3.0)` at x = 3 and `mod(x, 5.0)` at 5, 10 and 15, which xost
  works out as x - y * floor(x * rcp(y)), gave y, not 0. GLSL's precision for a division allows it; xost keeps it, and
  the tests take such a mod of a value half past a whole.
- **Every pixel took its samples in its passes.** xsdk_test's raster grid, `vk_loop_sample_grid.frag` compiled on the
  console, 48 cells of 10 by 10 pixels: a march along a height map left at the pass the height it samples reaches its
  ray, so a group's pixels leave apart, sampling a mip chain of flat levels at a LOD each pixel and pass gives and at
  gradients the same across a quad, and reading a tint at the counter through a0; then a loop whose passes are the
  same across a quad, sampling at gradients each pass works out, at the GPU's LOD, and taking a derivative: 0 of
  283,200 pixels wrong over 59 frames. The console's compiles of it, `vk_loop_sample_lod.frag`,
  `vk_loop_sample_grad.frag`, `vk_loop_sample_computed.frag`, `vk_loop_derivative.frag`, `vk_loop_parallax.frag` and
  `vk_vertex_loop_sample.vert` matched the PC's.
- **A scalar input is spread over its register before it's read.** Its interpolant or attribute fills x alone, and
  xost had read the whole register, so a loop counted by `int(v_count)` gated each component on its own lane; the
  console's compiles of `vk_scalar_input_loop.frag`, `vk_scalar_input.frag` and `vk_vertex_scalar_input.vert` matched
  the PC's.
- **Break build:** samples in a pass gathered before the loop got all 283,200 pixels wrong, and the seven shaders
  sampling in a pass compiled unlike the PC's.
- **Nothing else moved:** of milestone 45's corpus, only step 6's new shaders and three vertex shaders reading `int()`
  of a float attribute, one copy longer each, compile differently; Dusklight's pack is the same. vulkan4xenon_test
  passed 455 of 455 twice, vkbench 14 of 14, and Dusklight's title scene ran at its frame rates before.

## Loops through the layer, milestone 46 step 7 (2026-10-18)

vulkan4xenon_test draws milestone 46's loop shapes through the layer, each shader from the bundle and again compiled on
the console, each a strip whose pixels take their passes from their place: breaks and continues on data in an unrolled
loop (`loop_break.frag`) and in a hardware loop, its counter after it (`loop_count_break.frag`); loops nested 3 deep,
two in an if/else on data (`loop_nest.frag`); and samples in a pass, at a LOD each pixel works out and at gradients
the same across a quad, left on the level sampled, its count from data to 297 (`loop_sample.frag`). Logs:
`results/2026-10-18-milestone-46-step-7/`.
- **Every scene drew as worked out:** vulkan4xenon_test passed 487 of 487 twice, its 32 new checks among them, and the
  shader cache kept the 14 shaders compiled on the console.
- **Break build:** the layer writing the loop constant of loops whose count is data one pass short failed the samples
  scene's four colour checks and nothing else.
- **Nothing else moved:** xsdk_test passed 447 of 449, the 2 the controller checks with no pad; vkbench 14 of 14; and
  Dusklight's title scene ran at its frame rates before, its pack the same.
- **Milestone 46 is done:** each of docs/scope/46-loops.md's "Done when" holds, vulkan4xenon's PARITY.md has its Loops
  row Yes for xost and the layer, and a `discard` or `return` in a loop is still refused, as the scope left it.

## Instancing on the console, milestone 47 step 1 (2026-10-18)

`tests/probe_instancing.c`, its shaders packed by `tools/gen_instancing_probe.py` with xost's encoders, run on the
console through the console agent: every case passed, as docs/scope/47-instancing.md's design needs. Logs:
`results/2026-10-18-milestone-47-step-1/`.
- **A vertex fetch at a register an earlier exec of ALU alone wrote** reads where that register says: 64 quads, r0.x
  copied into r1 in the first exec and their positions and colours fetched at r1.x in the next, the fetches leading
  their exec with a vertex-cache bit each and the first of them serialized (case 1).
- **The design's prologue draws instances,** `k = floor(r0.x * c.x + c.y)`, `t.x = r0.x - c.z * k`, `t.y = k + c.w`,
  the vertex's attributes fetched at t.x and the instance's at t.y: 64 instances of a 6-vertex quad auto-indexed, and
  64 copies of a 4-vertex quad's 6 indices moved on by 4, from instance 5, every cell right (case 2).
- **The division holds at the counts that leave it the least room,** 1/V worked out on the CPU: a point a vertex, at V
  3, 7, 255, 257, 4,369, 21,845 and 65,535 auto-indexed and in copies moved on by 2 to 2^23, 32-bit indices up to
  16,646,148 among them, each point on its pixel, 14 of 14 counts (case 3). No rcp is involved.
- **What an instance costs,** GPU time from kick to fence over 2,000 instances' difference, and CPU time recorded:

  | An instance of | as the layer draws one now | a draw, the group between | one draw | copies of 8 | of 64 | of 512 |
  |---|---|---|---|---|---|---|
  | vkbench's 4-pixel cube, GPU | 0.289 us | 0.290 | 0.144, auto-indexed | 0.096 | 0.097 | 0.096 |
  | a 6-vertex quad, GPU | 0.273 us | 0.231 | 0.025 | 0.051 | 0.033 | 0.027 |
  | either, CPU | 692 to 728 clocks | 464 | 0 | 79 | 10 | 1 |

  The cube's 36 corners auto-indexed cost twice its 24 vertices indexed, as the copies keep a mesh's shared vertices.
  Copies of 64 meet the scope's "Done when": the cube at a third of the layer's GPU time, the quad at an eighth, and
  the CPU at a seventieth.
- **Write-combined memory reads at 21 MB/s with 32-bit loads and 80 MB/s with `vec_ld`,** 64 KB the best of 8 (case
  5): 0.8 ms a 64 KB index buffer, which the layer repeating an app's indices pays once a mesh, not a draw.
- **Break builds** each failed what they aim at and nothing else: the prologue without its floor case 2, 13 of the 14
  counts and the drawn check of the 12 ways the instanced shader draws; instances fetched at the vertex all 14; and
  copies not moved on the indexed half of case 2, the 7 indexed counts and the copies of 8 and up.
- **The probe's first run drew none of the layer's way,** as its batches were recorded before that way's streams were
  set: `xsdk_gpu_set_vertices` sends a stream's whole fetch-constant group as xsdk keeps it, so the recorded stream 1
  writes carried the stream 0 of the case before. Set first, every cell drew.

## Instanced vertex shaders in xost, milestone 47 step 2 (2026-10-18)

xost lowers a vertex shader that reads `gl_InstanceIndex`, or one compiled with the `instanced` option, with step 1's
prologue in an exec of ALU alone before its fetches, every fetch at t.x and `gl_InstanceIndex` t.y, from the instance
group the runtime writes, `XSDK_XSC_INSTANCE` (src/xsc_lower_vertex.c, `prologue`). Until step 4, vulkan4xenon draws an
instance at a time with the group (0, 0, 0, the instance), so the prologue keeps the vertex's index and gives the
instance. Logs: `results/2026-10-18-milestone-47-step-2/`.
- **The prologue drew through the layer:** vulkan4xenon_test's instancing rows, `instanced.vert` from the bundle
  rebuilt with this xost and its plain shader without `gl_InstanceIndex`, `vkCmdDraw`, `vkCmdDrawIndexed` and both
  indirect draws from instances 1 to 5, every frame right; vulkan4xenon_test passed 487 of 487 twice.
- **Nothing else moved:** xsdk_test passed 447 of 449, the 2 the controller checks with no pad; of milestone 45's
  corpus only the shaders reading `gl_InstanceIndex` compile differently, and of vulkan4xenon's bundles only
  `instanced.vert`; Dusklight's pack is the same. vkbench passed 14 of 14, and Dusklight's title scene ran at its
  frame rates before.
- **Off the console,** `tests/test_instancing.py` ran every vertex of N instances of V and of copies of a quad's
  indices on xost's simulator, and the division for every V to 65,535 and S to 2^23 under three roundings, the least
  room left 0.4854/V; the prologue without its floor failed the simulated draws.

## Instanced draws in xsdk, milestone 47 step 3 (2026-10-18)

`xsdk_gpu_draw_instanced` draws a list's instances in pieces of whole instances up to 65,535 vertices and a strip's or
a fan's an instance a piece, the group written before each piece; `xsdk_gpu_repeat_indices` writes a mesh's indices in
copies moved on by S, the power of two above its largest, 16-bit while they fit, for `xsdk_gpu_draw_indexed_instanced`;
an element marked `per_instance` is fetched at t.y (src/gpu_draw.c, src/shader.c). xsdk_test's `test_instancing.c`, its
shaders compiled by xost on the console, each check its own frame resolved and read back. Logs:
`results/2026-10-18-milestone-47-step-3/`.
- **12,000 instances of a 6-vertex quad in one call,** two pieces from instance 100, each an 8 by 8 cell of 1280 by
  720 placed and coloured by its instance's element and `gl_InstanceIndex`: every cell right, the 2,400 past them
  clear.
- **The same from a 4-vertex quad's indices,** in 64 copies 16-bit moved on by 4, and from a quad at vertices 69,996
  to 69,999 in 128 copies 32-bit moved on by 131,072: every cell right.
- **50 instances of a triangle strip,** an instance a piece: every pixel checked right.
- **A point a vertex at V 255, 257 and 21,845,** as many instances as a packet holds: all 196,605 points on their
  pixels.
- **Break builds** each failed what they aim at and nothing else: instances fetched at the vertex all three grids; 1/S
  taken for S + 1 the auto-indexed and 16-bit grids and the points, the 32-bit copies' indices lying where that floor
  still lands right; pieces after the first without their first instance the two-piece grid; copies not moved on the
  indexed grids; strips drawing their first instance alone the strips.
- **Nothing else moved:** xsdk_test passed 459 of 461, the 2 the controller checks with no pad; vulkan4xenon_test 487
  of 487 twice, vkbench 14 of 14, and Dusklight's title scene ran at its frame rates before.

## Instanced draws through the layer, milestone 47 step 4 (2026-10-19)

A vulkan4xenon pipeline with a binding at the instance's rate takes its vertex shader's instanced variant, from the
bundle or compiled on the console; its lists draw in one `xsdk_gpu_draw_instanced`, and its indexed lists of more
than one instance from the app's indices repeated into 64 copies, read from mapped memory 16 bytes a load; strips,
fans and draws past a packet go an instance at a time (vulkan4xenon:src/queue.c, src/transfer.c). Logs:
`results/2026-10-19-milestone-47-step-4/`.
- **Every instanced row drew as worked out,** vulkan4xenon_test passing 492 of 492 twice: `vkCmdDraw`,
  `vkCmdDrawIndexed` from index 8, both indirect draws, a shader without `gl_InstanceIndex` through its instanced
  variant, a strip instanced, and a list of 7 vertices an instance with its 7th never drawn.
- **1,000 instances in one `vkCmdDraw` added 22 dwords to the stream,** each in its cell in its colour.
- **Break builds** each failed what they aim at: `firstInstance` left out of the group the 7 rows that start past
  instance 0; copies from the index buffer's start the two indexed rows; the 7-vertex list concatenated whole its row;
  a draw an instance for lists the 1,000 instances' dwords alone. The plain variant where the shader lacks
  `gl_InstanceIndex` failed its row, the 7-vertex list's, the variant's count and the shader cache's, and also the
  indirect rows and the strip's: its attributes at the instance's rate, fetched at each vertex's index with their
  stream's stride, stretch its triangles over theirs.
- **Nothing else moved:** xsdk_test passed 459 of 461, the 2 the controller checks with no pad; vkbench 14 of 14;
  Dusklight's title scene ran at its frame rates before, its pack the same, and the layer takes the copies' memory
  only at the first draw that needs it.

## Instancing measured, milestone 47 step 5 (2026-10-19)

The probe's cost case through the milestone's own code, case 6 of `tests/probe_instancing.c` (xost's
`vk_instance_rate.vert` plain and instanced, `xsdk_gpu_draw_instanced` and `xsdk_gpu_draw_indexed_instanced` with 64
copies), and vkbench's `instances_4000`: `draws_4000`'s cubes, each one's matrix and tint at the instance's rate, in
one indexed draw through each backend. Logs: `results/2026-10-19-milestone-47-step-5/`.
- **What an instance costs**, against a draw an instance as the layer drew one before the milestone:

  | | as the layer drew one | milestone 47 | of it | Done when |
  |---|---|---|---|---|
  | the 4-pixel cube, GPU | 0.285 us | 0.072, indexed in 64 copies | 0.25 | at most 0.5 |
  | the 6-vertex quad, GPU | 0.273 us | 0.026, auto-indexed | 0.09 | at most 0.2 |
  | the cube, CPU recorded | 689 clocks | 10 | 0.015 | at most 0.1 |
  | the quad, CPU recorded | 735 clocks | 0 | 0.0006 | at most 0.1 |

- **vkbench's 4,000 cubes in one draw drew `draws_4000`'s picture** through both backends, 0 of 2,304 points apart,
  and vkbench passed 17 of 17. The medians, a frame:

  | | draw+submit | from submit to the fence | the fence from the first draw |
  |---|---|---|---|
  | xsdk, `draws_4000` | 1,162 us | 806 | 1,968 |
  | xsdk, `instances_4000` | 85 | 991 | 1,076 |
  | vulkan4xenon, `draws_4000` | 3,857 | 371 | 4,228 |
  | vulkan4xenon, `instances_4000` | 150 | 989 | 1,139 |

  vkbench's gpu figure runs from the submit's end to the fence, and `draws_4000`'s GPU works through its long submit,
  so that figure alone is larger for `instances_4000`; its fence lands sooner by the frame, at 0.55 and 0.27 of
  `draws_4000`'s. A frame's stream fell from 180,416 and 244,454 dwords to 1,848 and 1,865.
- **Nothing else moved:** xsdk_test passed 459 of 461, the 2 the controller checks with no pad; vulkan4xenon_test
  492 of 492 twice; Dusklight's title scene ran at its frame rates before.
- **Milestone 47 is done:** each of docs/scope/47-instancing.md's "Done when" holds; lists draw in one draw, indexed or
  not, and strips, fans and draws past a packet go an instance at a time, as the scope left them.

## The scalar unit's constant forms, milestone 48 step 5 (2026-10-19)

Step 5's first console run drew three of Dusklight's pixel shaders unlike the baseline and every pixel of xsdk_test's
`vk_loop_grid.frag` grid wrong where xost co-issued `mulsc` and `addsc` (results/2026-10-19-milestone-48/README.md,
"Step 5"). `probe_scalar_forms` (tests/probe_scalar_forms.c, its cases from tools/gen_scalar_forms_probe.py) runs 695
pixel shaders, each loading r0 to r63 from constants whose every lane holds its own value, running one instruction and
reading back what its scalar or vector op wrote. Log: `results/2026-10-19-milestone-48/probe_scalar_forms.log`.

| Form | Pass | Fail |
|---|---|---|
| `mulsc`, `addsc`, `subsc` | 124 each | 36 each |
| `adds`, `muls`, `maxs`, `mins` | 30 each | 0 |
| `subs` | 26 | 0 |
| `floors`, `truncs` | 14 each | 0 |
| `frcs`, `maxs` copies | 10 each | 0 |
| `rcp`, the control | 2 | 0 |
| the vector half beside a form | 19 | 0 |

- **A constant form whose constant's component is its temp's reads the temp for both.** `mulsc`, `addsc` and `subsc`
  read a constant's component in src3's lane w and a temp's in lane x (xenia:src/xenia/gpu/ucode.h:950-962). Every case
  whose two components were one failed, x and x or w and w, 36 a form, and the GPU gave the temp with itself under the
  one negation: `mulsc c0.x, r1.x` gave r1.x * r1.x, `addsc c1.w, r2.w` r2.w + r2.w, `addsc -c13.x, -r0.x`
  -r0.x - r0.x, and `subsc` 0. Every case whose components differ passed: temps r0 to r63 through each bit of the index
  the opcode's bit 0, src3_sel and the swizzle's bits 2 to 5 spread (ucode.h:1992-1995), c0, c1, c13 and c105 in every
  lane, negated or not, alone and beside a vector op of two temps, of a temp and a constant, and of two constants. y
  and y, and z and z, weren't run; xost takes them as the same.
- **Xenia doesn't show it:** its interpreter reads the constant whatever the lanes
  (xenia:src/xenia/gpu/shader_interpreter.cc:655-671), and xost's simulators, its model, ran step 5's programs as their
  vector ops. Everything else is as that model says, bit for bit, both orders of a max's and a min's operands among it.
- **What step 5's programs read:** the three pictures that changed used `mulsc r0.w, c0.w, r1.w`, `mulsc c2.w, r1.w`
  and `mulsc c4.x, r1.x`, and `vk_loop_grid.frag` `addsc r15.z, c13.z, r9.z`, each with equal lanes; the programs whose
  pictures matched used `subsc c105.y, r0.z`, `mulsc c145.w, r0.y` and `subsc c105.y, r1.z`. vulkan4xenon_test's six
  failures, `branch_data.vert`, `branch_uniform.vert` and `loop_break.frag` compiled on the console, held such forms as
  the PC compiles them.
- **xost never writes one** (src/xsc_optimize.c, `scalar_form`; src/xsc_program.c, `register_kinds`): it reads a
  literal from another lane of its register that holds the same number, as a splat's lanes do, keeps an op on a
  uniform in its temp's lane in the vector unit, and leaves the temp in its lanes when it packs registers. xost's
  simulators (xdis/simulate.py, xdis/equivalence.py) read the temp for both, and with them the probe's cases expect what
  the console gave, all 695.
## Counted loops end in bdnz, milestone 52 (2026-10-19)

With the spec's `-mpowerpc64`, GCC 15's `rs6000_preferred_doloop_mode` gave a counted loop's counter the word mode, 64
bits, which `doloop_end` refuses for 32-bit code (gcc/config/rs6000/rs6000.md:13686-13696; gcc/loop-doloop.cc:756-778),
so every counted loop ended in `addi`, `cmpdi` and `bne` instead of `bdnz`. gcc-xsdk.patch makes it the pointer mode.
Logs: `results/2026-10-19-milestone-52/`.
- **A title runs in 32-bit mode:** `probe_ctr` read MSR 0x9030, SF clear, and `bdnz` counted only CTR's low word,
  3 passes from 0xFFFFFFFF00000003 as from 3. So a counter moved to CTR from a 32-bit value is safe whatever its high
  word holds.
- **What it changed:** xsdk_test's loops ending in `cmpdi` went from 234 to 6, Dusklight's from 4,724 to 1,147 (SDL3,
  prebuilt with the old toolchain, holds 1,111), and code got slightly smaller. Of probe_unroll's loops typical of game
  code, 8 multiply-add chains ran 6% faster and a word copy 5%, a 4x4 matrix times vectors 3% slower, the rest within
  1%. Dusklight's CPU frame didn't change: its hot loops aren't counted loops of this kind.
- **Every title passed as before** with the new toolchain: xsdk_test, vulkan4xenon_test and vkbench.
- **Unrolling, measured again,** libxsdk and libvulkan4xenon alone with `-funroll-loops`: vkbench's CPU 0 to 5% less,
  Dusklight's the same, libxsdk's code a quarter larger and tools/xenia_scan.py's count in it from 2 to 44 (`rldic`,
  which Xenia skips). It stays off, as milestone 35 left it.
- **What an atomic costs** (`probe_atomics`), one thread: a relaxed `__atomic_fetch_add` 54.7 ns, acquire or release
  69.5, acq_rel 84.7, seq_cst 124.5; `sync` alone 53 ns, `lwsync` and `isync` 12. C++'s `operator++` on an atomic and
  `__sync_*` are seq_cst, so a counter that needs no ordering costs less than half as much asked for as relaxed. Two
  threads on one core contending: 146 to 173 ns relaxed, 285 seq_cst.

## XAM's saves, profiles and asynchronous calls, milestone 51 (2026-10-19)

`tests/probe_xam.c` ran its unattended cases with nobody signed in, presenting the bar from a thread of its own
throughout (logs: `results/2026-10-19-milestone-51/`, `probe_xam.log` and, under a second title ID,
`probe_xam_t2.log`).

- **An overlapped call** returns 0x3E5, `ERROR_IO_PENDING`, with the overlapped's result already 0x3E5 and its context
  0x000106A0. When done, XAM writes the result, length and extended error, sets the event if there is one, and puts
  the context back to 0. Without an event the call completes just the same.
- **`XamGetOverlappedResult` (XAM 0x1FB)** with its third argument 0, straight after the call: 0x3E4,
  `ERROR_IO_INCOMPLETE`, in under a microsecond, the length left alone. Once the call is done, 0 and 1 both give its
  result and length, with an event or without one; 1 straight after the call, on a thread of its own, returned once
  the call was done.
- **Errors come in the overlapped:** `XamContentClose` of a root not mounted completed with 0x65B and extended error
  0x80070002, `XamContentGetDeviceState` of device 0x7F with 0x65B and 0x8007048F. With no overlapped,
  `XamContentGetDeviceState` returned 0 at once.
- **With nobody signed in,** `XamUserGetSigninState` gives 0 for users 0 to 3; `XamUserGetXUID` for user 0 gives
  0x80070525 with type masks 1, 2, 4 and 7, the XUID set to 0; `XamUserGetName` gives 0x525, writing only a zero
  first byte; `XamUserGetSigninInfo` gives 0x80070525 with flags 0 to 2, the 40 bytes zero but byte 0x17, 0xFE.
- **The device enumerator** for saved games, 8 devices, asked for 0x280 bytes and gave the hard disk, device 1, type
  1, "Hard Drive", 154,888,372,224 bytes, and the USB stick, device 3, type 0x10, "USB Storage Device", in one pass;
  the next pass gave 0x103. `XamContentGetDeviceData`, `XamContentGetDeviceName` and `XamContentGetDeviceState` agreed.
- **Saves need no profile for users 0xFE and 0xFF.** `XamContentCreateEx` on device 1, open always, made each package
  (disposition 1) in 32 to 98 ms; `fopen("save:\probe.dat")` wrote 26 bytes, `XamContentFlush` took 49 to 93 ms,
  `XamContentClose` 0 to 2; open existing found it again (disposition 2) at once, and it read back as written. For
  user 0 with nobody signed in it completed with 0x65B, extended error 0x80070525, and its content enumerator
  returned 0x525. The enumerators for 0xFE and 0xFF each list both packages, with the display names given.
- **Each title ID has its own saves:** the packages are files of 61,440 bytes at
  `\Device\Harddisk0\Partition1\Content\0000000000000000\<title ID>\00000001\<file name>`. Under 0x5853444C the same
  names made new packages beside 0x5853444B's, and its enumerators listed only its own.
- **A package closed with a file open in it:** the close took 82 ms, 49 under the second title ID, and succeeded;
  the file's handle then failed writes and reads with 0xC000026E and closed fine, and the 7 bytes written before the
  close, with no flush, were there when the package opened again.
- **xsdk's helpers on the console** (`xsdk_test.log`): `xsdk_save_device` found the hard disk in 12 ms; a save for
  user 0xFE mounted in 187 ms, made new, flushed and closed in 75; mounted again in 15 ms it opened the package
  (disposition 2) and read back what was written; `XamContentDelete` took it away in 9 ms.
- **With a profile signed in** (`probe_xam_person.log`, `probe_xam_signed_in.log`): `XamUserGetXUID` gave the
  profile's XUID with type masks 1 and 7, and 0x80070525 with 2 and 4; `XamUserGetName` its gamertag;
  `XamUserReadProfileSettings` invert Y and vibration, 88 bytes, completing at once. User 0's save landed at
  `Content\<XUID>\<title ID>\00000001\<file name>`, and user 0's enumerator listed it, then the saves no profile owns,
  the first byte of their file names garbled in that second pass.
- **XAM's dialogs** (`probe_xam_person.log`, `probe_xam_dialogs.log`, `save_check_1.log`): a keyboard or message box
  asked for while XAM's UI is up returns 5 at once, `ERROR_ACCESS_DENIED`, and shows nothing. That includes the 0.3 to
  0.4 s after a dialog's call completes, before XN_SYS_UI says the UI closed, and the Guide being open. A message box
  with no overlapped returns 0x57; a keyboard with no overlapped showed, and its call hadn't returned 52 s after it
  closed. An accepted keyboard completes with 0, extended error 0 and length 0, its text in the buffer.
- **save_check** under title IDs 0x5853444D and 0x5853444E, a profile signed in: each made its save new, wrote,
  flushed and unmounted it, took the keyboard's text and a message box's button, 12 checks of 12 (`save_check_1.log`,
  `save_check_t2_1.log`); after a reboot, each found its own save, read it back as written and deleted it, 8 of 8
  (`save_check_2.log`, `save_check_t2_2.log`). The dashboard's storage view lists every xsdk title as an unknown game,
  the saves under it by the display names they were made with.
- **With nobody signed in,** save_check's save for user 0xFE was there after a reboot and read back as written
  (`save_check_nobody_1.log`, `save_check_nobody_2.log`). `XamContentDelete` of user 0's save fails with 0x65B while
  nobody is signed in.
- **A title's name in its `.xex`:** `tools/elf2xex.py --title-name` puts an XDBF database holding it in the first
  page, after the PE headers, as the resource named after the title ID (xenia:src/xenia/kernel/kernel_state.cc:111-125).
  The loader took it: `XexGetModuleSection` found the 118 bytes under that name (`cmake_check.log`), and 0xC0000225
  with no name (`cmake_check_noname.log`). XAM didn't copy it into a save: the title name in each package's header
  (offset 0x1691) stayed empty, as for a title with no name (`probe_xam_name.log`, title ID 0x58534450).

## Exports through the scalar unit, milestone 48 step 7 (2026-10-20)

xost now writes one lane of an export with the scalar op beside the vector op that writes the export's other lanes,
an alpha beside its colour, as Xenia's interpreter models an export's scalar op writing the vector op's register
(xenia:src/xenia/gpu/ucode.h:2041-2059; shader_interpreter.cc:911-939). The console agreed
(results/2026-10-19-milestone-48/, step 7): xsdk_test's tiles and instancing draws of `vk_frag_coord_rows.frag`,
`vk_instance_cells.vert` and `vk_instance_step.vert` passed, and all 103 of Dusklight's pixel pictures, 28 of them from
programs that changed, hashed as before, bit for bit.

## The pass after a depth clear, milestone 49 (2026-10-20)

`tests/probe_hiz_pass.c` drew xsdk's pass after a depth clear over other rectangles, one case a boot (logs:
`results/2026-10-20-milestone-49/`). Each case was checked with milestone 30's sequences after a clear and xsdk_test's
12 frames, and the last rounds added 64 rectangles of scattered sizes and places and 16 whose first block lies in the
top two rows of blocks, each the first draw after its own clear.
- **With no pass,** every scattered rectangle went wrong in the top right block of its first row of blocks, 6,560
  pixels over the 64, and 13 of the 16 in the top rows went wrong.
- **Where the pass lies decides which draws it puts right.** The top right block alone fixed nothing. Two blocks
  there fixed the 200x200 draw but left the full-screen draw's 199 pixels, whose first block they cover. Two at the top
  left passed the scattered rectangles but left 174 pixels wrong in a 64x100 draw starting in them.
- **The top 8 rows, the whole target's width,** put every pixel right in every check, for 0.99 to 1.13 us over a
  resolve clearing colour alone (269.9 us at 1280x720), where the whole target's pass took 29.4 to 29.5. xsdk draws the
  pass there since.
- **Skipped where the buffer stays off:** multisampled targets and targets past 1280x1024 get no pass
  (`xsdk_gpu_hiz_fits`).

## Multisampled formats and targets, milestone 44 step 3 (2026-10-20)

`tests/probe_msaa.c` on the console, its default group once and its pattern and depth cases each alone from a fresh
boot (docs/scope/44-tiling-and-msaa.md, step 3). Logs: `results/2026-10-20-milestone-44-step-3/`.
- **8_8_8_8, 2_10_10_10, its 7e3 float and 16_16_16_16 take 2 and 4 samples:** drawn, blended as at 1 sample, and
  resolved with every edge pixel at a step of the samples' average. **16_16_FLOAT, 16_16_16_16_FLOAT and 32_FLOAT
  don't average:** drawn right off the edges, their edge pixels resolved between the steps, a half-covered
  16_16_16_16_FLOAT pixel 0.625 1.0625 27.625 -98304 for 0.875 1.375 500.375 -0.5. xsdk refuses those three at 2 and
  4 samples (`xsdk_gpu_set_targets_samples`).
- **Tiles:** each format's held at its count, a 64-bit target's twice a 32-bit one's, and four targets at once
  resolved the non-float ones by value.
- **The sample pattern,** in 16ths of a pixel across, then down, from the pixel's centre: at 2 samples 0 at (-4, 4)
  and 1 at (4, -4); at 4 samples 0 at (-2, -2), 1 at (2, 2), 2 at (-6, 6) and 3 at (6, -6). Copy sample selects 0 to
  3 each copy one sample, and `PA_SC_AA_MASK` bit i, set in every nibble, lets sample i through. At 2 samples sample 0
  is the lower one, where Xenia takes it to be the upper (xenia:src/xenia/gpu/draw_util.h:315-320).
- **Depth resolves:** selects 0 to 3 copy one sample's depth; 4 to 6 average it, where Xenia copies one sample
  (xenia:src/xenia/gpu/draw_util.cc:717-757). xsdk's depth resolves copy sample 0 at every count.
- **Hierarchical depth at 2 and 4 samples,** at 1280x256 with xsdk's buffer and pass after a clear: hidden quads 84
  times as fast, every visible pixel drawn, and milestone 30's and 49's draws after clears all right. xsdk keeps it
  on up to 1280 wide and 1024 rows times samples.
- **Rates,** by the GPU's clock: fills 3.4 Gpixels/s at 1, 2 and 4 samples, 6.9, 13.8 and 27 samples a clock,
  blended as opaque; a 1280x720 resolve 270 us whole at 1 and 2 samples, and 277 in 3 bands at 1, 2 and 4.
- **On the console,** built clean at `654c07e` (local/m44p2/build.sh): xsdk_test 496 of 498, the controller's two
  failing with the pad idle, every new multisample and tiles check passing, the 720p frames tiled at 2 samples in 2
  and 3 bands and at 4 in 3 equal to their untiled references pixel for pixel with hierarchical depth on. Each break
  failed what it names, `msaa_bands2`'s colour wrapping past tile 2047 without hanging the GPU. vulkan4xenon_test 492
  of 492 twice, vkbench 17 of 17.
- **vkbench and code placement:** vulkan4xenon's `uniforms_1000` took 772 us against main's 670, but main with 256
  bytes of code added to gpu_draw.c took 676, and this build with 128 added 733; `states_1000` went 1277, 1300, 1262
  and 1317 over the same four. Padding alone moved them 4 to 5%, and nothing this step changed runs in their draws, so
  the 15% is most likely where code lands; a change in those two scenes needs such a control before it's put down to
  the code.

## Alpha to coverage, milestone 44 step 4 (2026-10-20)

`tests/probe_msaa.c`'s six alpha to coverage cases on the console, each alone from a fresh boot
(docs/scope/44-tiling-and-msaa.md, step 4). Logs: `results/2026-10-20-milestone-44-step-4/`.
- **`RB_COLORCONTROL`'s `alpha_to_mask_enable`, bit 4, works at 1, 2 and 4 samples,** and nothing hung. With it off,
  every sample was covered at every alpha.
- **Offsets 2, 2, 2, 2 (0xAA000017) cover every pixel of a quad alike:** the share of its samples nearest its alpha, a
  tie rounding up. The first alpha covering n samples: at 4 samples 2/16, 6/16, 10/16 and 14/16; at 2 samples 4/16 and
  12/16; at 1 sample 8/16, an alpha test at 1/2. xsdk's `XsdkBlend.alpha_to_coverage` takes these.
- **libxenon's 3, 1, 0, 2 (0x87000017) dither:** each pixel of a quad its own thresholds, the quad's total n/4 of its
  samples at alpha n/4 at every count.
- **The rule:** with offset o, a pixel's n-th of N samples is covered from alpha (4n - o) / (4N), as Xenia reads the
  offsets (xenia:src/xenia/gpu/registers.h:710-721). Samples are covered in index order, sample 0 first.
- **Offset i goes to the quad's pixel at column i & 1, row i >> 1:** under 3, 1, 0, 2 at 4 samples the top right
  pixel covered its first sample from 3/16 (offset 1) and the bottom left from 4/16 (offset 2). Xenia's code takes
  the column as the high bit (xenia:src/xenia/gpu/dxbc_shader_translator_om.cc:1881-1894), so it gives those two
  pixels each other's offsets; with 2, 2, 2, 2 it doesn't matter.
- **On the console** (xsdk_test built clean at `747d3b5`): 501 of 503, the controller's two failing with the pad
  idle, each count's squares covering the samples above at every alpha step and the colour control back to xsdk's
  once the target is set; `xsdk_test_atoc_noenable` and `_offsets0` each failed the three coverage checks alone.
  vulkan4xenon_test 492 of 492 twice and vkbench 17 of 17, vkbench's xsdk backend filling `XsdkBlend` by name.

## HLSL on the console, milestone 50 (2026-10-20)

xsdk_test's HLSL grid (`tests/test_hlsl.c`), its shaders compiled by xost from HLSL through glslang
(results/2026-10-20-milestone-50/):
- **VFACE** is +1 on a clockwise front face and -1 on a back one, D3D9's convention, from the same sign
  `gl_FrontFacing` reads.
- **VPOS** is each pixel's whole column and row as the pixel parameters' xy plus c255's zw, (0, the band's top):
  gl_FragCoord less a half, in a tiled pass too.
- **An ALU export with no write mask writes nothing:** a vertex shader's exports the pixel shader doesn't read go to
  interpolator 0 with no mask after `xsdk_shader_link`, and the pixel shader's own interpolator 0, written by another
  export first, came out right in every pixel, as Xenia describes the empty export (xenia:src/xenia/gpu/ucode.h:
  2041-2059).
- **A hardware loop an int in `$Global` counts,** from HLSL's loop in SSA form put back in the shape GLSL's takes, ran
  as its GLSL twin with each pixel leaving at its own pass.
- **D3D9's binding** (`step6_xsdk_test.log`): samplers at register(s3) and (s7) read the textures set at fetch
  constants 3 and 7; a bool an `if` reads, at boolean constant 128, took its arms through `cexec b128` and
  `cexec !b128`, the negated form's first run on the console; an int counting a loop ran through loop constant 16.
  With `register(b2)` and `register(i1)` kept (`step7_xsdk_test.log`), the bool took its arms through `cexec b130`
  and `cexec !b130`, the first boolean constant past 128 run on the console, and the loop counted through loop
  constant 17. A title's own CMake build (`step8_cmake_check.log`) drew `register(s2)`, `(b1)` at boolean constant
  129 and `(i3)` at loop constant 19, set as D3D9 sets them.
- **A loop-free pixel shader's if/else,** an if with no else and nested if/else, merged by the unroller's selects,
  drew what HLSL defines in every pixel (`step8_xsdk_test.log`). So did early returns put back where spirv-opt's
  merge-return took them from, and a switch as an if/else chain (`step8b_xsdk_test.log`).
- **A deferred kill:** `clip` before an if and a discard in an if inside another, each folded into one condition
  killed on where the shader ends, `kill_gt` or the comparison's own kill, left the clear in every killed pixel and the
  right colour in every live one (`step8c_xsdk_test.log`).
- **Skinning from int4 BLENDINDICES** fetched as UBYTE4, a0 loaded from each index lane in turn times a bone's 3
  registers, picked each corner's four float4x3 bones: the quad landed and shaded as the CPU skins it, and a palette
  with two bones swapped, or the indices unscaled, drew elsewhere (`step5_probe_skin.log`).

## A band's depth clear goes on a colour resolve, milestone 44 step 5 (2026-10-20)

vulkan4xenon's 720p pass tiled at 4 samples with depth in bands, the hierarchical buffer on in its 256-row bands, came
out right in band 0 and wrong from band 1 on, the first pass's draws missing, while the layer resolved each band's
stored depth last, so the band's depth clear, and the buffer's, went on a resolve of depth. Resolving depth first, so
the last colour resolve clears, put every pixel right; with depth last again the two checks failed alone, and with the
buffer off in every tiled pass nothing failed either way (vulkan4xenon:results/2026-10-20-milestone-17-part-2/). xsdk's
tiled passes resolve a band's depth before its colour since (`runs_at`, src/gpu_tiles.c). Milestone 17's first part's
2048x2048 shadow map with the buffer on lost pixels from its band 1 too, and cleared on a depth resolve the same way,
so its 2048 pitch, blamed above ("Render passes past eDRAM in the layer, milestone 44"), may not have been the cause;
it hasn't been run again, and the buffer stays off past 1280 wide.

## Multisampling's cost, and Dusklight at 2 and 4 samples, milestone 44 step 5 (2026-10-20)

Logs: results/2026-10-20-milestone-44-step-5/.
- **Samples cost fill nothing; bands cost vertices.** probe_tiles' fill-bound 720p frame took 10,302 us untiled at 1
  sample and 10,320 to 10,331 us at 2 and 4 samples in 2 or 3 bands; its vertex-bound frame 2,374 us untiled and 4,487
  and 6,594 us in 2 and 3 bands at any count, each band running every draw's vertices again. A band's fixed cost 6 us.
- **Through the layer, banding is the replay's cost.** Dusklight's logo shot replayed in 1.1 ms of CPU at 1 sample and
  in 29 ms with every pass with depth forced into 2 or 3 bands, the GPU's work past the replay 3 ms; at 2 and 4 samples,
  where only the EFB's passes tile, 11 and 13 ms of replay and 4 and 6 ms more GPU a frame than at 1 sample.
- **At 2 samples Dusklight dips under 30 fps:** the title scene 28.8 to 29.9 fps, Ordon's village 27 to 28 against 29
  to 30 at 1 sample. Its edges come out blended. Dusklight stays at 1 sample.

## The log over the network, milestone 53 (2026-10-20)

xsdk_test's log over UDP to `tools/net_log.py` (results/2026-10-20-milestone-53/):
- **Every line arrived,** in order, the whole log byte for byte, 67,379 bytes from the first line to `exit:`, including
  the lines written as the title ended. A build dropping every 100th datagram showed each gap on the PC.
- **A port nobody listens on** costs nothing: no `sendto` failed, and the title ran as without the network log.
- **What a line costs,** 100 lines timed: 0.31 ms to the file, up to 0.13 ms more over UDP.
- **`DbgPrint` costs 3 to 7 ms a line** on this console, and was 5.6 ms of a line's 5.9 while every line went to it,
  so the log leaves it off unless `dbgprint.txt` or `xsdk_log_set_dbgprint` turns it on. Why it's slow here isn't
  known.

## Drives, time zones and Xbox Home, milestone 54 (2026-10-20)

`tests/probe_drives.c`, `tests/probe_terminate.c`, `tests/terminate_check.c` and xsdk_test on the console, with a USB
stick in and no disc or memory unit (results/2026-10-20-milestone-54/):
- **Drive roots open as folders only with their closing backslash.** `\Device\Harddisk0\Partition1\` and
  `\Device\Mass0\`, the USB stick, opened and listed; without the backslash both answered 0xC0000103, not a directory
  (`probe_drives.log`). `\Device\Mass1`, `Mass2`, `Mu0`, `Mu1`, `BuiltInMuSfc` and `BuiltInMuUsb\Storage` answered
  0xC00002B6, no device; `\Device\Cdrom0`, with no disc, 0xC0000013; the `Mass<n>PartitionFile\Storage` paths and
  `BuiltInMuMmc\Storage` 0xC000003A, no such path. `\Device\Harddisk0\Partition0` is the whole disk, 0xC000014F as a
  folder.
- **xsdk's drive names reach them.** `hdd:\` listed 48 entries and held the `.xex` `game:` ran from, the same size
  and time; `usb:` listed the stick's 5 entries, with 32,768-byte units; `dvd:` and `mu:` failed with `ENOENT`
  (`step2_xsdk_test.log`). Mapping `hdd:` to the stick failed that check alone (`step2_xsdk_test_hdd_device.log`).
- **Their sizes,** `NtQueryVolumeInformationFile`'s size class: the hard disk's data partition has 16,384-byte units,
  9,453,636 of them; the USB stick 32,768-byte units; `Partition0` 512-byte sectors. A file of 65,537 bytes, written
  and flushed, took 5 units of `game:`'s (`step1_xsdk_test.log`).
- **`pwrite`, `fsync` and `statvfs` passed** their checks; the break writing at the descriptor's position failed
  `pwrite`'s alone (`step2_xsdk_test_pwrite_position.log`).
- **The hard disk brings a file's old bytes back when its end moves out again.** A 16-byte file cut to 9 bytes and set
  to 16 with the end-of-file class read back its old bytes 9 to 15, `line 2\n`, not zeros, its sizes right
  (`step2_xsdk_test.log`). With `truncate` and `ftruncate` writing the zeros themselves after setting the end, they
  read back zeros (`step3_xsdk_test_ftruncate_zeros.log`). A write past the end leaves the same: a byte written at 16
  past a 9-byte end left bytes 9 to 15 reading `line 2\n` again (`final_xsdk_test.log`), so a write starting past the
  end, `pwrite` there or `write` after `lseek`, now grows the file to it with zeros first.
- **The time zone is XAM's `GetTimeZoneInformation`'s** (0x43F), in Win32's layout. With no zone set on the console it
  answered 0 and wrote Win32's 172 bytes, all zeros (`step2_xsdk_test.log`), as `ExGetXConfigSetting`'s user
  category settings 1-7 had read 0 (`step1_xsdk_test.log`). With US Eastern set it answered 2, daylight time, and
  wrote 172 bytes: bias 300; "EST", bias 0, from month 11, week 1, Sunday, 2:00; "EDT", bias -60, from month 3,
  week 2, Sunday, 2:00, Windows' week-in-the-month dates. xsdk made it the `TZ`
  `<EST>5:00<EDT>4:00,M3.2.0/2:00:00,M11.1.0/2:00:00`, and `localtime` ran 14,400 s behind UTC, daylight saving on to
  newlib and to XAM (`step3_xsdk_test_eastern.log`, 546 of 546); the break giving the offsets the wrong sign failed
  that check alone
  (`step3_xsdk_test_tz_sign_eastern.log`, beside `ftruncate`'s, from before its fix). `XGetLanguage` gave 1 and
  `XamGetLocaleEx(111, 43)` 36, the US in XAM's locales, while the settings' own country read 103.
- **The title-terminate notification's routine runs on a thread of the console's,** hardware thread 3, not one the
  title made, when the title is ended from outside (`probe_terminate_registered_late.txt`):
  - every thread of the title is frozen: none counted in 1 ms, and a mutex a frozen thread held stayed held through
    20 ms of tries;
  - the title's file handles are closed, `NtWriteFile` 0xC0000008, and `game:` is gone: it wrote through
    `\Device\Harddisk0\Partition1\` instead; 54 bytes stdio held unwritten were lost;
  - it ran the 2 s it asked for and returned;
  - its r3 held 0x82070838, an address in the title's image, and r4 0. In a rebuild of the probe at both commits run,
    that's `__dso_handle`, not the registration, at 0x82070898; what the kernel passes there isn't known. Xenia's call
    of the routines, commented out, passes nothing (xenia:src/xenia/kernel/kernel_state.cc:489-504).
- **A registration left in place froze the console** as the next title, Aurora, started. With the routine taking its
  registration out before it returned, the console came back (`probe_terminate_unregistered_late.txt`). The
  registration lives in the title's memory, which the kernel frees once the routines return.
- **`xsdk_on_terminate` works so:** terminate_check's routine, the one registered last, wrote its marker through
  `\Device\Harddisk0\Partition1\` as the title was ended from outside, and the console went on to the dashboard; the
  next run read it back (`step4_terminate_check.log`). The build that never registers left no marker and failed both
  lines (`step4_terminate_check_noregister.log`).

## Letterboxing, milestone 54 step 5 (2026-10-20)

`tests/probe_letterbox.c` and xsdk_test on the console, its output 1920x1080 and widescreen, and once 480p at 4:3
(results/2026-10-20-milestone-54/):
- **AVIVO's external overscan doesn't place the picture.** radeonhd keeps a picture's aspect with
  `D1MODE_EXT_OVERSCAN_LEFT_RIGHT` and `_TOP_BOTTOM` (0x6588, 0x658C) as the bars and 0x659C, its guess at
  `D1MODE_CENTER`, at 1 (xf86-video-radeonhd:src/rhd_crtc.c:612-679, 721-790). All three read 0 before a title wrote
  them (`probe_letterbox_read.log`). Written with the rest of the scaler under its lock, 0x00880088 and 1, they read 0
  again after ten presents, while the scale into 808 lines written with them held (`xsdk_test_overscan.log`), so Xenos
  doesn't keep them there. The check that they went back passed only because they never changed, and the build that
  never wrote the bars failed the same line (`xsdk_test_overscan_nobars.log`). No one looked at the screen.
- **The kernel's scaler builder ends the title.** `VdInitializeScalerCommandBuffer` (xboxkrnl 0x1C5), asked for
  1280x720 to the 1920x1080 output with the filters `VdGetCurrentDisplayInformation` names (type 1, settings 0), a
  marker-filled buffer as its tenth argument and 1024 words of room, never returned: the log ends just before the
  call, and the console kept answering, with no reboot (`probe_letterbox_kernel.log`). xsdk doesn't call it.
- **Holding the display without `VdSwap` hung the console.** The probe's first fill case showed 1280x720 scaled to the
  whole output right, the ring round in the user's capture (`fill_16x9_1080p_fixed_surface.png`), through
  `xsdk_display_show` alone, then polled the controller every 50 ms with nothing pressed; the console stopped answering
  and needed a power cycle, the log ending at "showing" (`probe_letterbox_fill.log`). Why isn't shown; a title
  presents through `VdSwap` every frame ("The Guide shows through VdSwap"), as the probe now does.
- **So xsdk letterboxes by drawing,** and that works: on a 4:3 screen `xsdk_present`'s front buffers are 1280x960,
  the frame resolved from row 128 between black bars, and the scaler fills the output with them (src/present.c).
  Forced on the widescreen output, the display showed a 1280x960 surface with the scaler planned for it, the line
  counter reaching 960; the buffer held black above row 128 and from row 848 with the frame between on every row; and
  the scaler went back as the present closed (`step5_xsdk_test.log`, 549 of 549). The break resolving the frame at
  row 0 failed the bars' check alone (`step5_xsdk_test_lb_row0.log`).
- **On a 4:3 screen the frame keeps its shape.** The probe's fill case, presenting through `xsdk_present` on a 4:3
  480p output, showed 1280x720 in OBS's 800x600 capture as an 800x450 picture between bars of about 82 and 68 lines,
  128 and 112 rows of the 960, the circle round and the squares square (`letterbox_4x3_480p.png`).

## Windows-style objects and executable image memory, milestone 55 (2026-10-20)

The user's console ran `probe_objects` (18 passed, 0 failed) and `probe_exec` in `spike` and `query` modes.
The unchanged logs are in [results/2026-10-20-milestone-55](../results/2026-10-20-milestone-55/).

- Wait-any returned index 2; wait-all succeeded with three set events and timed out with one clear. Up to 64
  objects were accepted; 65 returned `C000000D`.
- Clearing a set event left it unsignalled. A pulse woke a waiter, reported previous state 0 and left the event
  clear. Signal-and-wait woke its worker.
- Recursive mutant acquisition and two releases succeeded. An unowned release returned `C0000046`; an abandoned
  mutant returned `00000080`. The probe did not separately record the other thread's release result.
- A one-shot timer signalled after 202422 time-base ticks. A periodic timer fired 100 times during the probe's
  window; after cancellation a wait timed out. This log does not establish every requested period's accuracy.
- Base priorities -16, -2, -1, 0, 1, 2 and 16 read back as requested. On one hardware thread, +2 performed 99% of
  the work and +0 less than 1%. Raised busy threads can starve other threads on that hardware thread.
- `ObReferenceObjectByHandle` with the imported `ExThreadObjectType` value returned a native thread object. Changing
  its affinity moved the worker from hardware thread 0 to 3. The pseudo handle and moving a sleeping thread were
  not separately established in this log; the new M55 checks cover them.
- A virtual allocation changed from read-write to read-only and back; the read-only query returned `00001002`.
  Decommit and release succeeded. Physical memory's protection changed from `00000404` to read-only `00000002`
  and read-write `00000004`.
- **The initial two-argument `NtQueryVirtualMemory` calls returned `C000000D`** for both the allocation and `.jit`,
  leaving the result zeroed. The three-argument follow-up below establishes the allocation query. `mmap` keeps
  its own reservation and committed-page bookkeeping for ownership; `munmap` releases a reservation after its last
  tracked page is decommitted.
- The singly linked list pushed and popped 1000 entries, then flushed a remaining entry and was empty. Concurrent
  pushes are covered by the new checks, not this probe.
- The 64 KB `.jit` pool at `82080000` reported protection `00000020`. Written code returned 42 after a flush;
  rewritten code returned 99 after a flush. Rewriting to 42 **without** a flush also returned 42 on this run.
  Logging occurred between the previous call and that rewrite and may have evicted the cached instructions.
  The observation does not isolate cache effects or establish that a flush is unnecessary. Host checks trace the
  C flush's exact line ranges and barriers; wrong-code variants test the console's return-value checks.

Steps 3–8 add SDK APIs and console checks on top of these results. Their new console checks, deliberate failures,
SDL, Dusklight and Vulkan regressions have not yet been run here. No new hardware result is claimed by a successful
cross-build or host mock test. GCC's builtin/libgcc cache flush and rebuild remain tracked in [TODO.md](../TODO.md).

M55 query follow-up (2026-10-20): the console ran `probe_query_memory` v1, retained unchanged as
[probe_query_memory.log](../results/2026-10-20-milestone-55/probe_query_memory.log), copied from
`Z:\Xbox-360-SDK\local\m55\logs\query\probe_query_memory.log`. It resolved ordinal 0x0EE and called
`NtQueryVirtualMemory(address, information, 0)` with a guarded seven-word result.

- All six allocation queries returned success with intact guards: the 64 KB committed read-write allocation,
  an interior address, its first 4 KB changed to read-only, the writable neighbour, the decommitted first page
  still reserved, and the free range immediately after release. The reported regions contained the address,
  their state and allocation base matched, and committed protection agreed with `MmQueryAddressProtect`.
- The read-only region was 4 KB with protection `00001002`; its neighbour was 60 KB with `00000004`.
  After decommit, state was `00002000` with protection zero. After release, state was `00010000`, allocation
  base zero and protection `00000001`.
- Title code and `.jit` returned `C000000D` with all seven words still `A5A5A5A5` and guards intact.
  `MmQueryAddressProtect` returned `00000020` for both image addresses. The log's **20 passed, 2 failed** reflects
  v1 incorrectly expecting those image queries to succeed. v2 checks for `STATUS_INVALID_PARAMETER` instead;
  this retained log is not a run of v2. The splash also passed.

The SDK restores the three-argument declaration and import for regions from `NtAllocateVirtualMemory`, including
`mmap`'s backing allocations (`src/mman.c`); this log did not separately exercise the `mmap` wrapper. Image
protection uses `MmQueryAddressProtect`. Pass zero as the third argument: its meaning and nonzero values remain
unverified. The earlier two-argument logs are retained as historical results.


## M55 acceptance failures (2026-10-21)

The [retained acceptance logs](../results/2026-10-21-milestone-55-acceptance/) include `m55_check` (51 passed,
four failed), the full SDK run and all 47 negative titles. M55 is not ready: the four baseline failures recur
in the full run. The full run also lacks network configuration and a connected controller for unrelated checks.

- A requested 5 ms periodic timer produced 16 firings once, 17 nineteen times and 18 twenty-eight times in the old
  100 ms loop, excluding the intentional 20 ms period. The one-shot baselines measured 5.67 and 5.99 ms.
  This is consistent with accumulated timer/scheduling lateness; it does not isolate the kernel's tick interval.
  The revised check measures 20 expirations with a 2 ms allowance per period and logs interval extremes and statuses.
- The event/semaphore/mutant/thread wait-all failed in all 49 M55 runs. Its old check omitted the returned status,
  so no kernel limit or API correction is established. Diagnostics now log setup, the combined wait, and each
  individual object after failure; the success assertion remains unchanged.
- Exact priority read-back from -16 through 16 failed in all 49 runs. The old check omitted the failing levels.
  The new sweep records each set/query result and read-back, and verifies restoration before continuing. It does
  not fake requested values or assume a clamping rule. The console must still establish the cause.
- Heap free-page counts were unchanged: 121074 in `m55_check`, 118946 in `xsdk_test`. Inspection of the compiled
  local baseline `test_pages.o` found no `malloc` or `free` call: GCC deleted the unused allocation. The old
  page counts cannot establish failed decommit. The revised test touches and verifies a volatile word on every
  4 KB page, records heap breaks and page counts, and still requires a full 32 MB return. Its compiled calls are checked separately; actual page
  return remains unverified until the next console run.

The separate [query v2 run](../results/2026-10-21-milestone-55-acceptance/probe_query_memory_v2.log) passed all 22
checks, including both image refusals and the splash. It does not settle the four acceptance failures above.

## M55 diagnostics v2 (2026-10-21)

The [unchanged console log](../results/2026-10-21-milestone-55-acceptance/m55_check_diagnostics_v2.log), supplied
from `local/m55/logs_check2/m55_check.log`, records 59 passes and one failure.

- Twenty requested 5 ms periodic firings took 118.561 ms; individual intervals ranged from 4.999 to 6.000 ms.
  The revised per-expiration allowance passed. Cancellation still timed out after its drain, as required.
- The touched 32 MB heap allocation reduced free pages from 121074 to 112874; free restored 121074 (8,200 pages).
  All 8192 touched words read back correctly. This verifies the corrected test and actual heap page return.
- All 33 priority set/query pairs succeeded, with `SCHED_OTHER` reported. The title thread's read-backs were:

  | Requested | Read back |
  |---|---|
  | -16 | -16 |
  | -15 through -4 | -3 |
  | -3 through +4 | unchanged |
  | +5 through +15 | +4 |
  | +16 | +16 |

  Restoration to the original 0 succeeded. The old all-level assertion was incorrect. The SDK's native calls stay
  unchanged; the test now checks these exact results. This observation is for the tested title thread; it does not
  establish clamping under other process base priorities. The +2 share and previous-priority checks also passed.
- Mixed event/semaphore/mutant/thread wait-all returned `STATUS_SUCCESS` in 2.646 microseconds and mutant release
  succeeded. Logging before the wait may have allowed the target to finish; that is an inference, not a measured
  explanation of the earlier failures. V3 explicitly separates completed-thread and gated pending-thread cases,
  logs after the pending interval, and releases the mutant on its acquiring thread. The wait result remains strict.

At this stage diagnostics v3 and its new wait-any negative had not run on hardware. This log cleared the
timer/heap questions for this focused run; the v3 hardware result follows below.

## M55 diagnostics v3 (2026-10-21)

The [unchanged console log](../results/2026-10-21-milestone-55-acceptance/m55_check_diagnostics_v3.log), from
`local/m55/logs_check3/m55_check.log`, records 67 passes and one failure caused by a zero-only wait-all assertion.

- With its thread already completed, mixed wait-all returned 0 in 2.887 microseconds.
- With the target behind a gate, both target and waiter polls returned `STATUS_TIMEOUT` before the gate opened.
  The combined wait returned `0x00000003` after 20.991 ms: `STATUS_WAIT_0 + 3`, the thread's position in the array.
  Its semaphore token was consumed and releasing the mutant on the waiter succeeded, confirming acquisition.
- WAIT_ALL can therefore return a nonzero object position too. V4 requires exactly `STATUS_WAIT_0 + 3` for this
  gated case, retaining the zero requirement for the ready control. No `src/` call uses wait-all; the change is
  to the test and public documentation, not an SDK status conversion.
- All 33 measured priority read-backs and restoration passed. The periodic timer passed at 118.394 ms for 20
  firings, and freeing the touched 32 MB heap allocation again returned 8,200 pages (112875 to 121075).

The retained log remains a v3 run with its original failure; it is not a run of the corrected v4 assertion.

## M55 acceptance (2026-10-21)

Diagnostics v4 passed on the console, with each break failing its listed checks
([results](../results/2026-10-21-milestone-55-acceptance/README.md#full-v4-run)).

**A thread at 16 that polls costs the threads it shares a hardware thread with.** Once priorities reached the kernel,
Dusklight's title scene lost about 2.5% in its heaviest windows. SDL's audio thread asks for TIME_CRITICAL, which SDL
turns into 16, and its xsdk driver polls with `SDL_Delay(1)`. Ignoring requests for 16 alone, or the driver asking for
NORMAL (SDL-xenon `d16871365`), brought the frame rate back; Aurora's own requests and the heap's decommit didn't
([runs](../results/2026-10-21-milestone-55-acceptance/README.md#dusklight-and-sdl_check)).

## XAM's sockets and loader, milestone 56 (2026-10-21)

Step 1 of docs/scope/56-store-networking.md, probe_net and probe_launch on the console
([results](../results/2026-10-21-milestone-56/README.md)).

**Sockets,** against tools/net_echo.py:
- The link read `0000000B` and the address 192.168.0.152 1 ms after `NetDll_XNetStartup`, `XNetGetTitleXnAddr`'s
  status `00000068`: no wait for the address on a wired console that's been on.
- With every setting 0, `NetDll_XNetGetOpt(1)` reads XAM's defaults: 32 stream sockets, 8 datagram, 16 KB buffers. The
  33rd TCP socket and the 9th UDP one fail with 10055 (WSAENOBUFS). Asking for 64 and 16 with 64 KB buffers gives 64
  and 16 with 63 KB; asking for 255 of each gives 232 of each, still 63 KB.
- On one socket, `SO_RCVBUF` asked for 64 KB reads 65536 and for 256 KB 261120; `SO_SNDBUF` reads 64512 above that.
- `SO_RCVTIMEO` in Winsock's milliseconds holds: a blocking `recv` from a silent PC returned -1 after 500 ms with
  10060 (WSAETIMEDOUT), twice on one socket. `SO_SNDTIMEO` reads back as set, but the probe's sends never filled the
  window (17,844 bytes went at once), so when a send times out isn't settled.
- `SO_LINGER` is Winsock's: two 16-bit fields in 4 bytes, the on/off first; unset, it reads `0000003C`. Winsock's
  {1, 0} reads back `00010000`, and after `closesocket` net_echo.py found the connection still open, neither reset
  nor closed. POSIX's 8-byte {1, 0}, as it comes, reads 1 and closes with a FIN.
- Binding 127.0.0.1 fails with 10049 (WSAEADDRNOTAVAIL), the socket marked insecure or not; a listener on 0.0.0.0
  takes connections to 127.0.0.1 and to the console's own address.
- `NetDll_XNetCleanup` and a second `NetDll_XNetStartup` take new settings: 64 and 16 sockets and 63 KB buffers read
  back, 64 TCP sockets open, and an echo through the PC works after.

**The loader,** probe_launch and probe_launch_t2, title IDs 0x5853444B and 0x58534451:
- `XamLoaderLaunchTitle` takes `game:\probe_launch_t2.xex` and the bare `probe_launch_t2.xex`. Given
  `\Device\Harddisk0\Partition1\xsdk\m56\probe_launch_t2.xex` or `\Device\Mass0\probe_launch_t2.xex`, it doesn't
  return and the console goes to the stock dashboard, the title never starting. Given a `.xex` that isn't there, it
  doesn't return either, and the title ends.
- It launches alike with the GPU and the display handed back first, as `exit` does, and with them still held.
- `XamLoaderSetLaunchData` returns 0 for every size up to the probe's 64 KB, but the launched title reads 1020 bytes at
  most, the data's first 1020, whole. Once read, `XamLoaderGetLaunchDataSize` returns 0x490 (ERROR_NOT_FOUND): the
  title it launches back gets none, and none is left after the dashboard.
- `ExLoadedCommandLine` holds the image's name and nothing else, in quotes (`"probe_launch.xex"`), whether the agent,
  another title or Aurora launched it. A title's arguments travel in launch data.

## Milestone 56 on the console (2026-10-21)

The checks of docs/scope/56-store-networking.md passed, each break failing its own
([results](../results/2026-10-21-milestone-56-acceptance/README.md)). What they showed beyond step 1's probes:
- **XAM launches through a link the title makes:** `\??\launch:` linked to `\Device\Harddisk0\Partition1\...` with
  `ObCreateSymbolicLink`, `XamLoaderLaunchTitle("launch:\launch_target.xex")` starts it, where the device path itself
  goes to the stock dashboard.
- **A {1, 0} linger closes without a FIN or a reset reaching the PC:** tools/net_echo.py found the connection still
  open after `close`, as in step 1, where a close without the linger sends a FIN.
- **Buffers of 63 KB in `NetDll_XNetStartup` read back as 65700 bytes** of `SO_RCVBUF` on a new socket.
- **Download rates,** curl_check over the wired 100 Mbit/s link, 8 MB from the PC in nine runs and 5 MB from
  speed.cloudflare.com, with XAM's 16 KB buffers and with `SO_RCVBUF` raised to 255 KB:

  | Download | XAM's buffers | Raised |
  |---|---|---|
  | HTTP from the PC, into memory | 10.9 to 11.6 MB/s | 11.4 to 11.6 MB/s |
  | HTTP from the PC, onto `hdd:` | 1.6 to 3.2 MB/s | 5.6 to 8.2 MB/s |
  | HTTPS from the PC, TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384, into memory | 1.84 to 1.87 MB/s | 1.86 to 1.87 MB/s |
  | The same onto `hdd:` | 0.80 to 1.04 MB/s | 0.86 to 1.22 MB/s |
  | HTTPS from Cloudflare, TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256, into memory | 1.06 to 1.19 MB/s | 5.3 to 8.0 MB/s |

  Plain HTTP into memory runs at the link. Raised buffers speed up what waits on the disk or a far host. HTTPS with
  AES-256-GCM stays at 1.9 MB/s whatever the buffers, where ChaCha20-Poly1305 reaches 5 to 8: the cipher, on the
  console's CPU, is what holds it back.

## Milestone 57 on the console (2026-10-21)

xsdk's text and imgui-xenon's checks passed, each break failing its own
([results](../results/2026-10-21-milestone-57/README.md)). What they showed:
- **Text, point sampled from an R8 atlas, is pixel-exact:** all 1,188 glyphs at scale 1, and scales 2 and 3, came out
  with 0 pixels wrong, as did blending over grey, the scissor and the vertex ring under a GPU held at each frame's
  start.
- **What text costs:** a 1280x720 screen of 4,608 glyphs, 36 strings, queued in 613 us of CPU and drew in 369 us of
  GPU. It first took 1,308 us: writing each glyph's 64 bytes as four 16-byte `vec_st` stores instead of sixteen 32-bit
  ones, and working out its atlas coordinates once rather than turning integers into floats each time, which goes
  through memory on this CPU, halved it.
- **Xenos's linear filter isn't exact at a texel's centre.** ImGui's font check drew its glyphs on whole pixels at one
  texel a pixel, which the check confirmed, so every pixel centre is a texel centre; sampled linearly, eight pixels
  still came out off by up to 8 of 255: 0xFF read 0xF7 and 0xFB beside 0, 0 read 0x03, 0xD8 read 0xD2, each pulled
  1/64 to 1/32 of the way toward the texel above or to the left, the same in two runs
  ([log](../results/2026-10-21-milestone-57/imgui/imgui_check_linear.log)). Point sampling gives the texels exactly, so
  the font check now samples point; ImGui titles keep linear filtering, as D3D9 titles on this GPU do.

## Audio's feeding, milestone 58 (2026-10-21)

Steps 1 and 2 of docs/scope/58-audio.md ([results](../results/2026-10-21-milestone-58/README.md)).

**Dusklight's crackling is late refills.** Over its title scene's 320 s, SDL-xenon's driver handed the kernel 634 silent
frames and clipped no sample. 599 came in three windows, 13 to 15, where Dusklight's `GetNewAudio` took up to 27.7 ms
and an `SDL_Delay(1)` up to 22 ms, against a ring of 3 frames, 16 ms, ahead; the device thread waited up to 7.8 ms for
Dusklight's JAS mutex there. The device thread ran on hardware thread 0, beside the game's main thread.

**The render driver,** from probe_audio:
- Its callback runs on hardware thread 4, in a kernel thread of its own, with the processor control region's DPC flag
  clear (0x150, xenia:src/xenia/kernel/xthread.h:73-86).
- It asks for a frame every 5.33 ms, 4.4 to 6.3 ms apart, and none before registering returns: 10 in the first 50 ms,
  the first after 1.2 ms. A second client registers too, and the two are called alike.
- `XAudioGetSpeakerConfig` returned 0 and the word `00010001` with the dashboard's audio at Dolby Digital 5.1, and
  `00000001` at stereo ([log](../results/2026-10-21-milestone-58/step7_speakers_stereo.log)): bit 16 is 5.1.

**Waking a feeder beside a busy thread** on hardware thread 1:
- Two busy threads at one priority share it in turns of 20 ms: each went up to 19,997 us without running.
- A feeder at the default priority woke up to 19.5 ms late after a 1 ms sleep, and 19.9 ms after a 5 ms periodic
  timer. At +1, +2 or 16, a 1 ms sleep woke within 1.9 ms and the timer within 5.9 ms, the busy thread losing 0.1%.
- **Set from the callback:** `NtSetEvent` on the title's handle returns 0xC0000008 there, as the callback's thread is
  outside the title's handles. `KeSetEvent` on the event's object, from `ObReferenceObjectByHandle`, works: a feeder at
  +1 or above woke within 3 us of each frame the kernel took, once a frame, the busy thread losing nothing measurable.
  At the default priority it still waited up to 19.9 ms.

**A 5 s wait on a thread's handle timed out within milliseconds**
([log](../results/2026-10-21-milestone-58/xsdk_test_join_timeout.log), xsdk_test at `aac0931`, which counted
the kernel's calls in `audio.c`). Each audio check opened the API, started a feeder on hardware thread 0, slept 100 ms
and then waited on the feeder's handle with `NtWaitForSingleObjectEx`, `USER_MODE`, not alertable, and a relative
timeout of 5 s:
- Each such wait returned `STATUS_TIMEOUT`, but the time base, read on hardware thread 0 before and after, advanced only
  0.66 ms, 12.6 ms and 0.51 ms over them; the ring check's feeder had ended 76 ms before its wait began.
- The kernel called back at its rate throughout, on hardware thread 4, with no failed submit, no call before the driver
  was stored, and the callback never caught inside: 18 or 19 calls in the first 100 ms, 18 to 21 over a 12.6 ms wait.
  The checks closed the device straight after the wait, which ended the calls, so they failed for want of time, not of
  calls.
- The closing check, whose main thread slept 50 ms instead, passed, as did sdl_check, faudio_check and Dusklight.

Why the wait timed out early isn't known. The audio checks now poll a thread's handle with a zero timeout and sleep
1 ms between.

## Milestone 58 on the console (2026-10-21)

xsdk's audio API, SDL-xenon on it and FAudio-xenon passed their checks, each break failing its own
([results](../results/2026-10-21-milestone-58/acceptance/README.md)). What they showed:
- **Dusklight no longer crackles:** its title scene, which handed the kernel 634 silent frames on SDL's polling driver,
  played 60,065 frames through `xsdk_audio_*` with none silent. The ring of 8 never fell below 5 queued, the longest a
  freed slot waited was 18.9 ms, and the scene ran 1,150.0 frames a second summed over its 32 windows, as before
  (1,150.1 to 1,151.0).
- **The API's own checks:** 95 frames in 500 ms with none silent; a feeder stopped 95 ms gave 9 silent frames of the 10
  due; beside a busy thread on its hardware thread, no frame silent, each freed slot refilled within 8 us, and the busy
  thread lost nothing; a close woke the waiting feeder within 4 us.
- **SDL3** opens the device as 6 channels of float at 48 kHz and mixes there without converting; its tone played 376
  frames, none silent.
- **FAudio** mixed 32 voices from 44.1 kHz into 5.1 in 1.37 ms of each 10 ms tick on average, 1.37 ms at most, its
  levels and its resampling exact (0.5000, 375.00 Hz), 413 frames with none silent.

## A small base with packed mips off, vulkan4xenon milestone 31 (2026-10-21)

Dusklight's bloom copies into a 20x11 texture, whose base xsdk lays out in the packed tail at (0, 16), where a resolve,
which writes from the corner, can't reach (vulkan4xenon:docs/ROADMAP.md, milestone 31). In Xenia a texture with the
fetch constant's packed mips off has no tail, its base at the corner whatever its size
(xenia:src/xenia/gpu/xenos.h:1281; texture_util.cc:251-253). xsdk and libxenon always set the bit, and Xenia was wrong
about small textures' tails twice before ("A small texture's mips share its base's tail", "A 2x1 texture's tail").

probe_mip_tail ran a third setup on the console: packed mips cleared in the fetch constant of tiled one-level textures
of 20x11, 11x20, 16x16, 32x16, 2x1, 4x1 and 1x1, a shape for each place the tail puts a base, each drawn one texel to a
pixel from four pages whose every texel names its page and place (log:
`results/2026-10-21-vulkan4xenon-milestone-31/probe_mip_tail.log`).
- **Every one read one rectangle of the base's page from its corner:** 20x11 from (0, 0) to (19, 10), 11x20 to
  (10, 19), 16x16 to (15, 15), 32x16 to (31, 15), 2x1 to (1, 0), 4x1 to (3, 0), and 1x1 at (0, 0). With packed mips
  on, the same shapes' bases sit at (0, 16) or (16, 0).
- **The two setups with packed mips on,** the control, read as on 2026-10-13, line for line
  (`results/2026-10-13-milestone-29/probe_mip_tail.log`).

So with packed mips off the console reads a small base at the corner, as Xenia does. `xsdk_texture_base_at_corner`
turns the bit off for a tiled 2D texture of one level whose base would sit in the tail; xsdk's layout then puts that
base at the corner for uploads and downloads (src/texture_layout.c), and the resolves take such a texture.
vulkan4xenon calls it for every colour attachment. Only tiled textures were probed, so a linear one isn't taken.

## Byte-reversed loads and stores cost what plain ones do (2026-10-23)

For a JIT running little-endian guest code on this big-endian host, `tests/probe_byterev.c` timed each byte-reversed load
and store beside its plain indexed form on hardware thread 0, its sibling idle: nine runs of about 40 ms each, each run
checking the bytes it moved (log: `results/2026-10-23-byterev/byterev.log`; with nine of its kernels broken on
purpose, exactly their checks failed, `byterev_break.log`). CPU clocks an operation, the median:
- **Integer loads:** `lhzx`, `lhbrx`, `lwzx`, `lwbrx`, `ldx` and `ldbrx` 1.6 each, eight independent a pass, as eight
  independent `add`s take. Chained, each load giving the next one's offset, `lwzx`, `lwbrx`, `ldx` and `ldbrx` 5.0 each.
- **Integer stores:** every form 16.3, `sthbrx`, `stwbrx` and `stdbrx` included. That's this kernel's bound, the same
  eight cache lines stored again each pass, not a store's usual cost.
- **A store then a load of the same word:** 48.1 a pair, whether either reverses bytes or not.
- **Into and out of the FPRs,** which have no byte-reversed load or store, so the value goes through a GPR and memory:
  - `lwbrx; stw; lfs` 34.8 against `lfsx`'s 3.3, and `ldbrx; std; lfd` 34.8 against `lfdx`'s 3.3.
  - `stfs; lwz; stwbrx` and `stfd; ld; stdbrx` 67.4 each, against `stfsx`'s and `stfdx`'s 16.3.
  - Chained, the reversing hop added 50 clocks: 124.2 against 74.3, for floats and doubles alike.
- **VMX:** `lvx` 3.3 alone, and 3.2 followed by a `vperm` that reverses each word or the whole vector. `stvx` took 16.3
  with or without a `vperm` before it.
- **The sibling:** a spinner of adds on hardware thread 1 counted 254 million passes a second while the main thread
  slept, and 177 million beside the main thread's adds, its `lwzx` loads and its `lwbrx` loads alike. The byte-reversed
  loads hold the other hardware thread up no more than plain ones.
- **`ldbrx` and `stdbrx` run,** as Xenia has them for 360 code (xenia:src/xenia/cpu/ppc/ppc_emit_memory.cc:640-690).
- **Calibration:** a chain of dependent adds took 2.0 clocks an add, eight independent ones 1.6.

So a JIT reaches little-endian guest memory through the byte-reversed forms at no cost for integers. A guest float
pays the trip between a GPR and an FPR through memory, about 31 clocks a load and 51 a store, while VMX reverses bytes
for nothing.

## Zero runs in a .xex, milestone 63 (2026-10-24)

Step 2 of docs/scope/63-homebrew-store.md: tools/elf2xex.py leaves an image's 4 KB pages of zeros out of the file, as
basic compression's `{data_size, zero_size}` blocks (xenia:src/xenia/kernel/util/xex2_info.h:345-348). xsdk_test's
10.94 MB image, packed five ways and run from `hdd:\XSDK\m63` by the console agent (logs:
`results/2026-10-24-milestone-63-step-1/`):
- **One raw block,** the packing before (`x_raw`): 634 passed, from 10.95 MB.
- **4 KB runs, the last block ending in the 6.6 MB of zeros at the image's end:** the launch returned as any does, then
  nothing; no log was made and the console was back on Aurora.
- **XexTool's** (`-c u`, its "uncompressed (no zeroed data)", from the file before; `x_tool`): two blocks, runs at
  32 KB, and the blocks stop short of the image's end, the last with no zeros: 634 passed.
- **32 KB runs as XexTool's, the zeros at the end in the last block:** nothing, as the second.
- **4 KB runs, the zeros at the end in no block** (`x_tail`): 634 passed, from 3.93 MB.

So the loader takes runs at 4 KB and fills what the blocks leave of the image with zeros, and starts no title whose
last block ends in zeros. Whether it's that block's place or its 6.6 MB run that it refuses isn't settled: the runs
between data blocks here were 44 KB at most, and SwanStation's 264 KB. elf2xex now ends the last block at its data.
So packed, xsdk_test passed 634 (`xsdk_test`; the 2 failures, as ever, the controller's with nobody at it), SwanStation
started from 4.97 MB, 31.98 MB of image, and read its games folder (`swanstation`), and curl_check downloaded a packed
launch_target and launched it (below). Xenia loads xsdk_test so packed: it zero-fills the whole image before copying the
blocks in (xenia:src/xenia/cpu/xex_module.cc:564-648).

## A store's update and its downloads, milestone 63 (2026-10-24)

Step 1 of docs/scope/63-homebrew-store.md, from `hdd:\XSDK\m63` and `m63c` (logs:
`results/2026-10-24-milestone-63-step-1/`):
- **A running title replaces its own `.xex`:** probe_replace is one ELF packed as "xsdk probe_replace 1" and "2". Started
  as version 1 with version 2 beside it as `probe_replace.new`, it opened its own `.xex` to read and to write, renamed
  the new one over it, and launched that path; what started there carried version 2's name (`probe_replace`). Started
  without the new one, it said so and launched nothing (`probe_replace_nonew`). The store updates itself as it updates
  any app, no second title needed.
- **A folder renames** with its files in it, as on 2026-10-14 ("Files renamed on the console").
- **A GitHub release's asset comes at 9 MB/s:** curl_check fetched Dusklight's 21.4 MB beta zip from github.com, which
  redirects to release-assets.githubusercontent.com, into memory with `SO_RCVBUF` raised: 2.32 s, 9006 KB/s, over
  TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256. Mbed TLS offers ChaCha20-Poly1305 first
  (mbedtls-xenon:library/ssl_ciphersuites.c:51-54), and from the PC (OpenSSL, TLS 1.2) both hosts take whichever of
  ChaCha20-Poly1305 and AES-GCM is offered first. One run, beside Cloudflare's 7.9 MB/s in the same run; curl_check's 13
  checks passed (`curl_check`).

## The store's catalogue and screenshots, milestone 63 (2026-10-24)

Step 4 of docs/scope/63-homebrew-store.md: the Homebrew App Store and its store_check, from `hdd:\XSDK\m63s` and
`m63u`, against tools/net_http.py `--site` serving the store's test site (logs:
`results/2026-10-24-milestone-63-step-4/`):
- **Mbed TLS takes tools/store_catalogue.py's signatures:** the catalogue signed by the tool's standard-library ECDSA,
  P-256 with RFC 6979's nonces, verifies with `mbedtls_pk_verify` on the console, and one with a byte changed or signed
  with another key doesn't. store_check passed 12 of 12, each of the seven breaks of the store's tests/breaks.py
  failing the checks it names.
- **A catalogue kept on `hdd:` through `.part`, `fsync` and `rename` comes back** when the address answers nothing, an
  older signed one is refused, and a signed new address is followed on the next fetch.
- **stb_image 2.30 decodes a 1280x720 PNG on the console,** which the store scales to 768x432 and keeps, so the next
  showing reads it off the disk without the network; one whose SHA-256 isn't the signed one is never decoded.
- **The store draws its three screens at 720 lines** with Archivo through imgui-xenon, identical frames on two runs
  (`store/`), and shows the kept catalogue and screenshot with a notice when the address answers nothing
  (`store_offline/`).

## Installing, updating and removing, milestone 63 (2026-10-24)

Step 5 of docs/scope/63-homebrew-store.md: the store's store_check from `hdd:\XSDK\m63s` against tools/net_http.py
`--site`, and the store from `m63v` (logs: `results/2026-10-24-milestone-63-step-5/`):
- **miniz 3.1.2 unzips on the console** from memory, its byte order set big-endian, each file streamed to its `.part`
  through a SHA-256 checked against the catalogue's; a package whose SHA-256 isn't the signed one is never opened, and
  one file signed with a wrong hash leaves nothing of the install behind.
- **A download cut at half is taken up with a Range:** curl's `CURLOPT_RESUME_FROM_LARGE` from what the writer thread
  had put in the `.part`, net_http.py answering 206 for the other 134 of 268 bytes.
- **The journal holds:** `app.json.pending` renamed `app.json.part` is the commit point; a cut install laid out before
  it is taken back on the next start and one after it finished, the rename replacing app.json and each file as on
  2026-10-14 ("Files renamed on the console").
- **`remove` takes an empty folder as well as a file,** newlib's `remove` reaching xsdk's `unlinkat` without
  `AT_REMOVEDIR` (src/path.c); xsdk has no `rmdir`, and a folder with something left in it stays, its unlink failing
  with 0xC0000101, `STATUS_DIRECTORY_NOT_EMPTY`.
- store_check passed 21 of 21, each of the fourteen breaks of the store's tests/breaks.py failing the checks it names.

## Launching, and the store updating itself, milestone 63 (2026-10-24)

Step 6 of docs/scope/63-homebrew-store.md: store_check from `hdd:\XSDK\m63s`, the store from `m63ya` and `m63z`
(logs: `results/2026-10-24-milestone-63-step-6/`):
- **An app the store installed starts from its own folder with the store's path as launch data:** `xsdk_launch` of
  `game:\apps\launch-target\launch_target.xex`, a folder inside the store's, with `ExLoadedImageName` and its 0, 63
  bytes; launch_target found the path and launched the store back by it, and the store started again with launch data.
  Dropped to no data, launch_target can't come back and says so.
- **A running title updates itself:** store_check 0.0.0.1 installed 0.0.0.2 over the .xex it ran from, through the
  `.part` files and the `app.json.pending` to `app.json.part` journal, then `xsdk_launch` of its own
  `ExLoadedImageName` started the new one, which read 0.0.0.2 from its .xex; the store did the same from 0.1.0.0 to
  0.2.0.0, its page then saying Running. Step 1 had shown the rename over a running .xex.
- store_check passed 23 of 23 over its three starts, each of the seventeen breaks of the store's tests/breaks.py
  failing the checks it names.

## Depth clamp, milestone 61 (2026-10-22)

Fast3D turns on `GL_DEPTH_CLAMP` (docs/scope/61-fast3d.md, "Depth clamp"). Xenos turns clipping off with
`PA_CL_CLIP_CNTL`'s `clip_disable`, bit 16 (xenia:src/xenia/gpu/registers.h:547), which Xenia takes as depth clamp
and as user clip planes off (xenia:src/xenia/gpu/d3d12/pipeline_cache.cc:885-886, 1508). xsdk_test's depth clamp
checks drew a rectangle at w = 1 across the near plane, z from -0.5 to 0.5, and one across the far plane, z from 0.5
to 1.5, depth cleared to 0.5 and compared ALWAYS, each with clipping on and off, then resolved its depth and read it
back (`results/2026-10-22-milestone-61/xsdk_test.log`):
- **With clipping on,** nothing beyond either plane is drawn, and the depth there stays 0.5.
- **With `clip_disable`,** the rectangles draw beyond both planes, their depth clamped: 0 (`000000`) where z is
  -0.2475, and 1 (`FFFFFF`) where it's 1.2525. Inside the planes depth is z, as with clipping on (0.2525, 0.7525).
- **User clip planes do nothing while clipping is off:** a plane set after `clip_disable` cut nothing, and the same
  plane set with clipping on cut as before.

So `XsdkRaster`'s `depth_clamp` clamps depth as OpenGL's and Vulkan's do, and a title can't have it and user clip
planes at once. With `clip_disable` never set (`tests/breaks/depth_clamp_off.patch`), the three checks with depth clamp
on failed and no other (`results/2026-10-22-milestone-61/xsdk_test_break_depth_clamp.log`). Both runs failed only the
controller's two checks besides, its pad off.

**It isn't their depth clamp, though: `clip_disable` stops all clipping,** where theirs stops only the near and far
planes. Starship drew through libultraship's Fast3D with it on, as Fast3D's OpenGL backend turns on `GL_DEPTH_CLAMP`
(lus:src/graphic/Fast3D/backends/gfx_opengl.cpp:668), and the console's captures of its intro showed triangles with a
vertex behind the camera drawn wrongly: in the corridor cutscene only the corridor's far end, in a box, its near walls
missing, and a flat triangle across the Arwing passing Corneria
(`results/2026-10-22-milestone-61/starship_clip_disable.png`). With clipping on and z clamped into 0 to w in the vertex
shader where w is positive, the same frames drew the corridor whole (`starship_clipping_on.png`). A title that wants
OpenGL's depth clamp does that; `depth_clamp` suits geometry that stays in front of the camera.

## Vertices written after an invalidate, milestone 61 (2026-10-22)

Fast3D on xsdk writes each batch's vertices into a ring of write-combined memory and draws them from there
(lus:src/graphic/Fast3D/backends/gfx_xsdk.cpp). A segment of the ring is written again only once the fence after its
last use has passed. To save a cache invalidate a draw, one version invalidated a 512 KB segment's whole range once,
before the first draw that used it, and started each draw's vertices on a 4 KB page of its own; the CPU went on writing
later draws' pages while the GPU drew earlier ones. On the console, in Starship's intro, one run drew a large flat
polygon that isn't in the game across the hangar shot
(`results/2026-10-22-milestone-61/starship_ring_invalidate_once.png`), and one run with `xsdk_gpu_invalidate` of each
draw's own range before it drew the frame as the game does (`starship_ring_invalidate_each_draw.png`). That was taken
for the GPU reading past a draw's range, and the version was reverted.

It wasn't that. The polygon was a cached vertex pointer that Fast3D read back as an offset, which draws one in about
half of all runs whatever the invalidate (next section). With that fixed, four runs of the once-a-segment invalidate
drew Starship's intro at frames 2950, 3025, 3100 and 3175 and the hangar at 4800 as the per-draw invalidate does, pixel
for pixel (`starship_vertex_cache.txt`, `starship_ring_invalidate_once_marked.png`). So nothing here shows the GPU's
vertex fetch reading past a draw's range; four runs don't rule it out either.

## Cached vertex pointers below 1 MB, milestone 61 (2026-10-22)

xsdk's heap starts low: `sbrk` reserves it with `NtAllocateVirtualMemory` at an address the kernel picks, and on the
console that's 0x00070000 (`results/2026-10-20-milestone-49/dusklight.log`, "the heap may grow to 438 MB from
0x00070000"). So a pointer into the heap can be below 1 MB, and code that tells pointers from small numbers by size
can't.

libultraship's Fast3D does. A `G_VTX_OTR_HASH` command holds an offset into a vertex resource; the first time it runs,
the interpreter writes the vertices' address over the offset, and after that takes any value above 0xFFFFF for that
address (lus:src/graphic/Fast3D/interpreter.cpp, `gfx_vtx_hash_handler_custom`). A resource below 1 MB has its address
read back as an offset and added to the address again, every time the command runs, until the sum passes 1 MB; from
then on its vertices come from that address, which holds something else. Where resources land differs from run to run
with the order the loader's threads allocate in, so the same build drew Starship's intro right in one run and with
the team's limbs stretched across the corridor in the next. Of four runs, three had one to four vertex resources below
1 MB and each of those drew stray polygons; each stray pointer was its resource's address times two or three
(`results/2026-10-22-milestone-61/starship_vertex_cache.txt`, `starship_cached_pointer_low.png`).

libultraship-xenon marks the cached address with bit 0 of the command's first word, which the command never sets
(starship:tools/Torch/src/n64/gbi-otr.h:132-133; all 4662 in Starship's archive have it clear). In six runs, three
with vertex resources below 1 MB, every cached pointer lay inside its resource and every frame drew as the game does
(`starship_cached_pointer_marked.png`).

## Starship's audio follows its updates, milestone 61 (2026-10-25)

Starship makes its audio a frame a game update: after each update and its drawing, the game wakes its audio thread
and waits for it (starship:src/port/Game.cpp, `push_frame`; src/port/Engine.cpp, `HandleAudioThread`). An update makes
two audio frames of 528 or 560 samples at 32 kHz, 560 while the queue holds fewer than libultraship's 2480
(lus:src/audio/AudioPlayer.h:12), so 30 updates a second make what the console plays and fewer make less. The game
updates 30 times a second and, interpolating to 60, draws its display list twice an update
(starship:src/port/Engine.cpp, `ProcessGfxCommands`).

On the console, counting each audio frame the game queued and what the queue held before it, over 100 s from boot
(`results/2026-10-25-milestone-61/`; the counters, since taken out, are `starship_diagnostics_*.patch` there):
- **At 60**, the default the port came with, the scenes that drew under 60 made 25.6 to 28.9 updates a second, so
  28,294 to 31,307 samples a second of the 32,000 played, and the queue was empty before an audio frame up to 92
  times in 10 s; in the windows at 59.1 to 60 fps its lowest was 0 to 1429 samples (`starship_audio_at_60.log`).
  The player hears it crackle.
- **At 30**, the updates held 30.0 a second in every window without a scene load, and the queue was never empty in
  them (`starship_audio_at_30.log`; `starship_default_30.log` with 30 as starship-xenon's default and the player's
  settings untouched).
- **Loading** held one update 130 to 440 ms, in the game's own work (once 351 ms in its audio), in each window where
  the game loaded a scene's resources: its animation loader logged and 29 to 46 textures were new. The queue ran dry
  one to four times in each such window at either rate (`starship_update_parts_at_30.log`). An update otherwise took
  2.5 to 13.8 ms of the game's work, 16 to 26 ms drawing and presenting, the present's wait for the frame's time
  included, and 3.7 to 5.5 ms of audio, one after the other.

The texture uploads in the heavy scenes, 0.45 to 0.79 ms a frame, are textures the game invalidates every frame: about
90 of them missed Fast3D's cache once a frame each, with the cache never full and nothing evicted
(`starship_texture_cache_misses.log`).

## A Fast3D draw's parts, milestone 61 (2026-10-25)

gfx_xsdk's `DrawTriangles` timed a part at a time with the time base, over Starship's first 100 s from boot at the
game's own 30 fps (`results/2026-10-25-milestone-61/starship_draw_parts.log`; the timing, since taken out, cost about
0.01 us a draw). In the windows of 87 to 104 draws a frame, a draw took, in us:
- **0.33 to 0.37 binding textures:** 78 to 95 of a frame's draws bind a texture, as an N64 display list changes texture
  with each part of an object; a bind writes 7 dwords, the texture's fetch constants (src/gpu_draw.c,
  `xsdk_gpu_set_texture`).
- **0.35 to 0.42 on the block of constants:** building and comparing it every draw, and writing its 3 registers in 29
  to 44 of them, when a texture's size or filtering changes. Its sizes were converted from integers each draw, which
  the in-order core does through memory; kept as floats with each texture, the block took 0.27 to 0.33
  (`starship_draw_parts_sizes_as_floats.log`).
- **0.26 to 0.28 on the invalidate,** 11 dwords, a WAIT_REG_MEM among them, every draw (src/gpu_draw.c,
  `xsdk_gpu_invalidate`).
- **0.19 to 0.20 submitting:** the vertices' fetch constant and the draw, the GPU layer's own cost.
- **0.14 to 0.16 switching programs and 0.08 to 0.09 on state,** for the 7 or so of a frame's draws that change them.

A part costs about what it writes into the ring, about 25 to 50 ns a dword, not what it decides. Invalidating the
vertex ring once a segment as it's first written, with each draw's vertices on pages of their own (previous section's
argument), took 0.22 to 0.31 us off a draw and left GPU time where it was. Frames 600, 1800 and 2400 then came out
byte for byte the same as with the per-draw invalidate, in two runs; frame 1200 differed, by as much between two runs
of the same build, in a patch of the planet's glow (`starship_invalidate_captures.txt`).

With both, the build of record's costs (`starship_draw_costs_final.log`), at 30 fps, a frame of 33.3 ms:
- **A draw less its copy,** 1.18 to 1.35 us in the windows of 58 to 104 draws, and up to 3.6 us in the title's 6,
  as a frame's fixed calls are shared among fewer draws.
- **The copy** about 3.2 GB/s: 165 KB a frame in 0.052 ms.
- **The frame's shares:** the backend 0.13 to 0.24 ms, under 1%; Fast3D's interpreter with the backend, from
  `StartFrame` to `EndFrame`, 5.2 to 7.0 ms, 16 to 21%; the GPU 4.5 to 7.1 ms. The game's own work, its audio and the
  present's wait are the rest (previous sections).
