# Pitfalls

What breaks a title on the console and not on a PC, or not in Xenia. Each gives what it looks like, what to do, and
where the evidence is; docs/HARDWARE.md is cited by section.

## Big-endian data

- **Looks like:** numbers, colours and coordinates from files or tables made on a PC come out scrambled.
- **Do:** read and write PC data a byte at a time or swap it (`__builtin_bswap16`, `32`, `64`); data the console
  writes for itself needs nothing. Code with a byte-order switch is built big-endian, and its swaps checked: a port's
  swap that runs whatever the host is turns the console's data around.
- **Evidence:** the `.xex` is PowerPC big-endian, machine 0x01F2 (SCOPE.md, section 2.2); the GPU takes big-endian
  words as the CPU wrote them ("Constants loaded from memory go in groups of 4 too"), and audio big-endian floats
  ("Runtime, milestone 2"); Dusklight's swaps that ran on any host (vulkan4xenon:docs/ROADMAP.md, "Milestone 9:
  Dusklight on Aurora's null backend").

## Plain stores to GPU memory

- **Looks like:** the title dies at a 64-bit store (`std`) into memory from `xsdk_gpu_alloc`, and its log just stops.
- **Do:** write GPU memory through `volatile uint32_t *` stores, a word at a time, as the samples do, or copy into it
  with `memcpy`, xsdk's, which keeps to the stores it takes. GCC merges plain stores into 64-bit ones, and one that
  isn't 8-byte aligned faults in write-combined memory. VMX `vec_st` to an address checked 16-byte aligned is fine.
- **Evidence:** "A ring without VdInitializeEngines"; "Copies into write-combined and cached memory"; "The SDK's
  memcpy, milestone 20".

## Writes the GPU doesn't see

- **Looks like:** a draw shows what the memory held before: last time's vertices or colours, now and then.
- **Do:** after the CPU writes vertices, indices or texels, `xsdk_texture_upload`'s included, call
  `xsdk_gpu_invalidate` over them before the draws that read them; and don't rewrite memory a queued draw may still
  read, waiting for its fence first, as samples/triangle does.
- **Evidence:** "The pacing modes pace, but small vertex invalidates don't take"; "Fewer dwords a draw, milestone 19".

## Constants in groups of four

- **Looks like:** the GPU hangs at the first draw: `xsdk_gpu_wait` and `xsdk_present_begin` return 0xC00000B5,
  `STATUS_IO_TIMEOUT`, and the screen stops.
- **Do:** set float constants through `xsdk_gpu_set_vertex_constants` and `xsdk_gpu_set_pixel_constants`, which write
  whole groups of 4. Constants the GPU loads from memory (`xsdk_gpu_load_*_constants`) go in whole groups of 4, from a
  multiple of 4, from 64-byte aligned memory, and a group loaded isn't also set. Pixel constants 252 to 255 are xsdk's.
- **Evidence:** "Shader constants: partial writes hang the GPU, libxenon's whole blocks don't"; "Float constants go in
  groups of 4"; "Constants loaded from memory go in groups of 4 too"; `include/xsdk/gpu.h`.

## 64 KiB GPU allocations

- **Looks like:** GPU memory runs out long before the sizes asked for add up to it.
- **Do:** each `xsdk_gpu_alloc` takes whole 64 KiB pages, so allocate a few large blocks and place small buffers in
  them; shaders' microcode already shares pages. A block starts on a 64 KiB page; start each vertex stream and index
  list in it on 4 bytes, each texture on 4 KB and constants the GPU loads on 64 bytes. A vertex stream at an address
  off 4 bytes is read, without a word, from the dword below it. docs/GUIDE.md, "Memory the GPU reads", has the rules.
- **Evidence:** src/gpu.c, `allocate`; "Shaders' microcode in shared pages, milestone 25", where 201 shaders took
  3,216 of the kernel's 4 KB pages at an allocation each, and 16 sharing them; TODO.md, "Small GPU allocations other
  than shaders"; for the boundaries, xenia:src/xenia/gpu/xenos.h:1113-1134 and "Indexed draws and topologies,
  milestone 12".

## A mutex locked after it's freed

- **Looks like:** a crash some time later in `free` or `malloc` (`_free_r`), far from the bug.
- **Do:** destroy a mutex, and free the memory holding it, only once no thread can lock it again: join or wake and
  stop the threads first. A `pthread_mutex_t` points to a critical section made at its first lock, and once its memory
  is freed the lock takes newlib's free-list link for that pointer. A PC only touches dead memory there.
- **Evidence:** "Dusklight runs the title scene"; TODO.md, "Mutexes holding their critical section".

## Sleeps that take twice as long

- **Looks like:** a loop paced with 1 ms sleeps runs at half the speed meant; short timeouts last longer.
- **Do:** a sleep takes up to a millisecond more than asked: 1 ms took 1.84 to 2.00 ms, and 2 ms 2.05 to 3.00 ms.
  Pace frames with `xsdk_present_begin`, wait for the GPU with fences, and keep sleeps for waits where a millisecond
  more doesn't matter. `xsdk_gpu_set_wait_spin` has fence waits spin instead.
- **Evidence:** "Sleeps, write speeds, and a set-up that wouldn't run from a cached stream"; "SDL's drivers and the
  fixed clocks on the console"; "Fence waits, and drawing after a resolve, milestone 34".

## Files that share nothing

- **Looks like:** opening a file that's open already fails, `open` with `EBUSY` from the kernel's 0xC0000043, a
  sharing violation; `std::filesystem::remove_all` fails on a folder holding folders. Xenia allows both.
- **Do:** open a file once and keep the handle, or close it before opening it again; remove nested folders from the
  inside out, closing each first. Only the log is opened shared, for reading.
- **Evidence:** "The log, read while it's open, milestone 27"; TODO.md, "A folder removed while it's open".

## Absolute POSIX paths

- **Looks like:** `/data/level1.bin` isn't found, though `data/level1.bin` sits beside the `.xex`.
- **Do:** use paths relative to the title's folder, `game:\`, which is the current directory at start, or name the
  drive, `game:/data/level1.bin`. A path starting with `/` goes to the kernel as a device path, such as
  `\Device\Harddisk0\Partition1\...`.
- **Evidence:** src/path.c; docs/PARITY.md, "Files, drives and storage".

## xost's GLSL subset

- **Looks like:** the build stops with `xost didn't compile it (refused): ...`, or worse, a shader that builds gives
  wrong values: `sin` and `cos` far from -pi to pi, `atan` past -1 to 1, integer division of negative numbers, and
  `mod` at some exact multiples, such as `mod(3.0, 3.0)` giving 3.
- **Do:** keep to what xost lowers: no bitwise operations or `switch`; angles reduced to -pi to pi first; loops with
  at most 255 passes; at most 64 registers a shader. docs/GUIDE.md, "Shaders and their constants", says what else the
  build takes.
- **Evidence:** xenos-toolchain/docs/xsc.md, "Approximate builtins", "Register budget" and "Not yet / out of scope".

## Floats at unaligned addresses

- **Looks like:** an alignment exception at `lfs` or `lfd` reading a float out of a packed struct or a byte stream.
- **Do:** nothing, built with xsdk's GCC: it moves floats through integer registers where it can't tell they're
  aligned. Code built by another compiler, or assembly, must keep float loads aligned.
- **Evidence:** "Dusklight runs the title scene"; "Floats at any address, milestone 23".

## Data held in LR

- **Looks like:** a 64-bit value comes back with its high word zeroed, about once a millisecond.
- **Do:** nothing, built with xsdk's GCC, which keeps LR to 32-bit values; assembly mustn't park data in LR.
- **Evidence:** "The console keeps only LR's low word across an interrupt".

## Stack overflows

- **Looks like:** the console hangs and needs restarting; the log just stops.
- **Do:** keep big arrays off the stack, `static` or on the heap; the main thread and others get 1 MB unless they ask
  for more.
- **Evidence:** "How the kernel hands a title its faults, milestone 40's probe"; "The kernel's calls into a title
  keep its registers" and "What the kernel leaves a title", for the 1 MB.

## Ending under the Guide

- **Looks like:** a title that ends by itself while the Guide is open hangs as it ends.
- **Do:** end on a button the title reads, as the samples end on Back, which XAM keeps from the title while the Guide
  is open. Leaving through the Guide's Xbox Home is fine.
- **Evidence:** "The Guide, milestone 37"; TODO.md, "The Guide, beyond milestone 37".

## Xenia's differences

- **Looks like:** a check that passes in Xenia fails on the console, or the other way round.
- **Do:** take Xenia as a crash and smoke check only, and the console's log as the answer. docs/GETTING_STARTED.md,
  "Xenia, as a smoke check", lists what Xenia gets wrong, among them the instructions it skips without a word, which
  each build's `xenia_scan` lines name by function.
- **Evidence:** CODE_GUIDELINES.md, section 12, and the Xenia sources each item there names.
