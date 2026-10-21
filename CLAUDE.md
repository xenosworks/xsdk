# Xbox 360 SDK

A from-scratch homebrew SDK that builds `.xex` titles for the retail kernel and dashboard on an RGH console, with a GPU
layer that drives Xenos directly. Scope, sources and milestones are in SCOPE.md.

## Working rules

- Follow CODE_GUIDELINES.md for all code. If a request conflicts with it, say so before writing code.
- Agree on scope before starting a feature. Build only what was agreed, and don't build ahead. The agreed scope is
  SCOPE.md.
- Back up any claim about the kernel, the XEX format or Xenos with evidence: Xenia, libxenon or xost source (under
  `xenos-toolchain/reference/`, cited as in SCOPE.md), other public reverse engineering, or a hardware result. Say
  which. No Microsoft tooling, no leaked SDK material, no OpenXeChain.
- Hardware runs are on the user's RGH console. The user copies the `.xex` over FTP, launches it, and sends back the
  log. Ask for a hardware run only when a milestone or a question about the real kernel needs one.
- xsdk_test's network checks talk to `tools/net_echo.py` on the PC; `net_host.txt` beside the `.xex` holds the PC's
  address and the port, as "192.168.0.125 7370". Without both, those checks fail.
- Xenia Canary (`C:\Users\Mena\Downloads\xenia_canary\xenia_canary.exe`, which may be moved) is for crash and smoke
  checks only. Launch it with the `.xex` path; it stays open after the title exits, so stop it after a few seconds.
  A title's log lands in its `cache` folder, named after the `.xex` (`cache\xsdk_test.log`), and `DbgPrint` output
  in its `xenia.log`; the log goes to `DbgPrint` only with a `dbgprint.txt` beside the `.xex`. For the GPU tests add
  `--readback_resolve=full`, so the CPU sees what resolves write, and `--async_shader_compilation=false`, or Xenia
  skips draws while it compiles a new shader. Xenia has no display controller, so the display checks fail there. It
  runs the time base at the 50 MHz the kernel reports, not the console's 49.875 MHz, so the check that the SDK's
  clock keeps the kernel's time fails there too (docs/HARDWARE.md, "The time base runs at 49.875 MHz"). Its
  `MmQueryStatistics` guesses reserved bytes and counts free pages in its physical heaps only, so the heap
  reservation and heap growth checks fail there as well (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:500-525).
  It runs each of the title's threads on a host thread of its own (xenia:src/xenia/kernel/xthread.cc:378), so two
  busy threads placed on one hardware thread don't share it there, and that placement check fails too. It ignores
  `NtCreateFile`'s share access (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:102-145), so the check that the log
  can be read while it's open passes there even broken on purpose. Its `NtSetInformationFile` takes the rename class
  and renames nothing (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:202-251), so the rename checks fail there. Its GPU registers hold what was last written
  (xenia:src/xenia/gpu/graphics_system.cc:181-205), so the performance counter that times fence waits never counts,
  and the check that a wait returns soon after its fence fails there, as do the checks that the GPU's clock counts and that leaving a hardware loop early costs nothing. Its `NetDll_XNetDnsLookup` is a stub
  (xenia:src/xenia/kernel/xam/xam_net.cc:542-560), so the check that `getaddrinfo` looks a name up fails there too.
  In m55_check it fails the manual pulse, which pulses a host event (xenia:src/xenia/kernel/xevent.cc:65-68), the
  non-owner's mutant release, which it doesn't check against the guest owner (xenia:src/xenia/kernel/xmutant.cc:41-54),
  and the pending wait-all, as it returns 0 from every wait-all (xenia:src/xenia/kernel/xobject.cc:290-298). Its
  `KeQueryBasePriorityThread` returns the host thread's priority, which `KeSetBasePriorityThread` picks from five bands
  (xenia:src/xenia/kernel/xthread.cc:717-736), so the priority sweep fails and, its restore failing too, m55_check ends
  there, before the semaphore, page and written-code checks.
  Its shaders run their control flow as one loop on the address (xenia:src/xenia/gpu/spirv_shader_translator.cc:567-614)
  and leave a hardware loop pixel by pixel (spirv_shader_translator.cc:986-1002), so a quad's pixels that leave a loop
  at different passes run what follows apart, and the raster check of samples in loops' passes, whose second loop takes
  a derivative and the GPU's LOD, fails there in blue.
- Xenia skips instructions it doesn't implement, silently (xenia:src/xenia/cpu/ppc/ppc_emit_*.cc,
  `XEINSTRNOTIMPLEMENTED`). GCC emits some of them: `mcrf`, rare now that the toolchain reserves CR2-CR7, and `rldic`
  under `-mpowerpc64`. Each link runs `tools/xenia_scan.py`, which lists such instructions by function; treat any Xenia
  result that goes through those functions as unreliable.
- Xenia finds a function's end by scanning, and a forward `b` makes it take everything up to the target as one function
  (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204). Tail calls into that range then run the wrong code or crash Xenia. The
  toolchain turns off tail calls and hot/cold splitting, and `src/xsdk.ld` puts import stubs first; keep it that way.
- Write memory the GPU reads (write-combined or uncached) through `volatile` 32-bit stores. GCC merges plain stores
  into 64-bit `std`, and on the console one that isn't 8-byte aligned raises an alignment exception there
  (docs/HARDWARE.md, "A ring without VdInitializeEngines"). The one exception is VMX `vec_st` to an address checked
  16-byte aligned, as the float constants are written (src/gpu_constants.c; docs/HARDWARE.md, "Copies into
  write-combined and cached memory").
- The console keeps only LR's low word across an interrupt, so the toolchain's GCC keeps LR to 32-bit values
  (gcc-xsdk.patch), and assembly mustn't hold data in LR either (docs/HARDWARE.md, "The console keeps only LR's low
  word across an interrupt").

- Milestone 55's Xenia run cannot establish priority share, hardware-thread placement, or physical pages returned:
  it schedules host threads and estimates memory statistics. Use the console checks in `m55_check.xex`/`xsdk_test`.
  `NtQueryVirtualMemory(address, information, 0)` works on allocated virtual regions but returns C000000D for
  title code and `.jit` (results/2026-10-20-milestone-55/probe_query_memory.log). Use `MmQueryAddressProtect` for
  image protection. Xenia ignores the third argument and cannot validate that ABI; its image results are not
  console evidence. Only a zero third argument is established.
  Xenia's `InstrEmit_icbi` emits a no-op (xenia:src/xenia/cpu/ppc/ppc_emit_memory.cc:1132-1135), so a rewritten
  function may keep its translated old code and the JIT rewrite checks may fail there; this is not console evidence.
  The probe's no-flush observation had intervening logging that may have evicted code. Use host cache-operation
  traces to reject missing flushes and wrong-code variants for deterministic console return checks.
  GCC's builtin/libgcc cache-flush work is in TODO.md, retained by the user; do not patch or rebuild GCC here.

## Build

- The toolchain runs in WSL Debian: GCC 15, binutils and newlib for `powerpc-xsdk-elf`, installed at `~/xsdk-gcc`.
  `tools/toolchain/build.sh` builds it (`JOBS=32` uses every core); its specs and patches say what differs from a
  stock build. Call WSL from PowerShell with `wsl.exe -d Debian --exec bash -c '...'`; Git Bash rewrites its absolute
  paths.
- clang-format is at `C:\Program Files\LLVM\bin\clang-format.exe`.
- Build the SDK and the test title (`build/xsdk_test.xex`) from PowerShell:

```
wsl.exe -d Debian --exec bash -c 'cd /mnt/z/Xbox-360-SDK && make'
```
