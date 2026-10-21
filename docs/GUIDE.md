# Guide

xsdk's calls by task, in the order a title makes them, each task with the sample that does it. docs/GETTING_STARTED.md
gets a title built and onto the console, and docs/PITFALLS.md says what breaks one there. The headers in
`include/xsdk/` give every call a line of its own; this page says how they fit together.

- **Statuses:** calls that wrap the kernel return its `NTSTATUS`, which `NT_SUCCESS` tests (`kernel.h`). Log a failure
  with the call's name and the status as `0x%08X`, as the samples do. The C and POSIX calls set `errno` as POSIX says.
- **Threads:** the GPU layer, `gpu.h`, `shader.h`, `present.h`, `splash.h` and `display.h`, holds no locks: call it
  from one thread at a time, the one that draws, called the drawing thread below. `xsdk_gpu_alloc`, `xsdk_gpu_free`
  and `texture.h`, which only allocate and copy, may run on any thread, as may logging, reading controllers, writing
  memory the GPU isn't using, and the C and POSIX calls.
- **Undone by:** what puts the state back, or ends what the call started. The title's exit closes the display and the
  GPU and gives them back to XAM, whatever the title left open (src/exit.c).

## A title's life

[samples/template](../samples/template/main.c), [samples/hello](../samples/hello/main.c)

xsdk's start, `xsdk_start` in `libxsdk.a`, opens the log, sets the console's time zone, runs the static constructors
and calls `main(1, argv)`, `argv[0]` empty. What `main` returns is the exit status; returning, or `exit`, runs `atexit`
handlers and destructors, closes the display and the GPU, logs `exit: status <n>` and goes back to the dashboard
(src/start.c, src/exit.c). A failed `assert` or `abort` logs `exit: signal 6` and ends the title the same way.

Xbox Home, or the console agent sending the console to the dashboard, ends the title without `exit`: no `atexit`
handler or destructor runs, and what stdio holds unwritten is lost, so a title flushes what it must keep as it goes.
`xsdk_on_terminate(routine)` (`terminate.h`) has a routine run then, on a thread of the console's with every thread of
the title frozen, its file handles closed and `game:` gone. It may take no lock the title's threads could hold and use
no stdio, `malloc` or other newlib state: only raw kernel calls, on device paths such as
`\Device\Harddisk0\Partition1\...`, in well under the 2 s the console gave it. xsdk takes the routine's registration
out once it returns, and at exit, as one left in place froze the console as the next title started (docs/HARDWARE.md,
"Drives, time zones and Xbox Home, milestone 54"; tests/terminate_check.c).

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_splash_show(ms)` | nothing: fades the logo in, holds it `ms` from the start, lets the GPU and the display go | itself | one, before `xsdk_gpu_open` |
| `xsdk_gpu_open()` | the GPU taken from XAM's D3D device, its ring and xsdk's set-up | `xsdk_gpu_close`, or exit | drawing |
| `xsdk_splash_begin()` | the logo up on a 1280x720 display, the GPU open | `xsdk_splash_end`, or `xsdk_present_open` taking the display over, the logo staying until its first frame | drawing |
| `xsdk_present_open(mode, buffers, interval, &w, &h)` | the display at `w` by `h`, 1280x720 in `XSDK_DISPLAY_720P`, scaled to the output; 2 or 3 front buffers, 1280x960 on a 4:3 screen, the frame on rows 128 to 847 between black bars; frames paced to vblank, each shown `interval` vblanks | `xsdk_present_close`, or exit | drawing |
| `xsdk_present_begin(&address, ms)` | the GPU address to resolve the next frame into, once its buffer is off screen; 128 rows into the buffer where letterboxed | `xsdk_present_end` | drawing |
| `xsdk_present_end()` | the frame queued, shown at a vblank once the GPU has finished it | | drawing |
| `xsdk_present_close()` | nothing: waits for the queued frames, closes the display, frees the buffers | | drawing |
| `xsdk_gpu_close()` | nothing: the registers xsdk changed written back, the GPU XAM's again | | drawing |

The frame loop, as samples/hello runs it:

1. read the controller, and leave the loop on Back;
2. `xsdk_present_begin`, which paces the loop: it waits for a buffer to come off screen;
3. draw into eDRAM and resolve into the buffer's address (Textures, render targets and resolves, below);
4. `xsdk_present_end`.

`xsdk_present_begin` fails, `0xC00000B5`, when no buffer comes off screen in time: the GPU has hung, as it does on a
broken command (docs/PITFALLS.md). `XSDK_DISPLAY_720P` gives 1280x720, but the kernel's 640x480 on an interlaced
output, so a title draws at the size `xsdk_present_open` returns. While the Guide is open, XAM draws each frame under it
by itself, `xsdk_present_stats`'s `system_frames` counting them, and keeps Back from the title (docs/HARDWARE.md, "The
Guide, milestone 37"). `xsdk_present_stats` and `xsdk_gpu_stats` count flips, missed vblanks and the ring's waits, to
measure a title by. `display.h` is the display without the pacing, for a title that shows buffers itself.

On a 4:3 screen the frame is letterboxed: each front buffer is 1280x960, black but for the 1280x720 frame on rows 128
to 847, and the scaler fills the screen with it, so the frame keeps its shape. The title still draws 1280x720:
`xsdk_present_begin`, `xsdk_present_pixels` and `xsdk_present_buffer_address` give the frame's address, not the
buffer's. `xsdk_display_force_letterbox` is a test hook that letterboxes on any screen, which squashes the picture on
a widescreen one (docs/HARDWARE.md, "Letterboxing, milestone 54 step 5").

## Memory the GPU reads

[samples/triangle](../samples/triangle/main.c), [samples/cube](../samples/cube/main.c)

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_gpu_alloc(bytes)` | write-combined physical memory in whole 64 KiB pages, or `NULL` | `xsdk_gpu_free` | any |
| `xsdk_gpu_address(memory)` | nothing: the address the GPU knows the memory by | | any |
| `xsdk_gpu_invalidate(address, bytes)` | the GPU's vertex and texture caches dropping that memory, before the draws after | | drawing |

- **Write it with volatile 32-bit stores** or xsdk's `memcpy`; never plain stores, which GCC merges into 64-bit ones
  that fault there (docs/PITFALLS.md).
- **Invalidate it after the CPU writes it,** and before the draws that read it. A resolve invalidates what it writes.
- **Don't write what a queued draw still reads.** `xsdk_gpu_fence` submits what's queued and returns a fence's
  number, and `xsdk_gpu_wait(fence, ms)` waits until the GPU has passed it; `xsdk_gpu_fence_done` asks without waiting.
  A frame's buffer from `xsdk_present_begin` is off screen, so the GPU is done with the frame that last used it.
- **Allocate few, large blocks:** each allocation takes whole 64 KiB pages, and starts on one (src/gpu.c,
  `allocate`).
- **Place buffers in a block on these boundaries:**
  - **a vertex stream on 4 bytes,** as its fetch constant holds the address in dwords, the low 2 bits being the
    constant's type, so an address off 4 bytes is taken as the dword below it, and its size as whole dwords too
    (xenia:src/xenia/gpu/xenos.h:1113-1126; libxenon:drivers/xenos/xe.c:1181-1185). Strides and element offsets are
    whole dwords, a stride up to 255 of them, which `xsdk_shader_set_vertex_layout` checks
    (xenia:src/xenia/gpu/ucode.h:737-742). The console draws streams 144 bytes apart in one block
    (tests/test_anisotropy.c; docs/HARDWARE.md, "Anisotropic filtering, milestone 33").
  - **an index list on 4 bytes:** 16-bit indices need 2 and 32-bit ones 4, which `xsdk_gpu_draw_indexed` asserts
    (xenia:src/xenia/gpu/command_processor.cc:1338-1342), and the console drew lists of both, each starting on 4 bytes
    of one block (tests/test_indexed.c; docs/HARDWARE.md, "Indexed draws and topologies, milestone 12").
  - **a texture on 4 KB,** for `xsdk_texture_attach` (`texture.h`; xenia:src/xenia/gpu/xenos.h:1129-1134), and
    constants the GPU loads on 64 bytes (Shaders and their constants, below).
- **Reading it back is slow,** as the CPU doesn't cache it; read only what's needed.

Memory only the CPU uses comes from `malloc`, whose heap can grow into all the memory the title has free
(docs/HARDWARE.md, "The heap reserves the title's free memory").

## Shaders and their constants

[samples/triangle](../samples/triangle/main.c), [samples/cube](../samples/cube/main.c), HLSL in
[tests/cmake](../tests/cmake/cmake_check.cpp)

Shaders are GLSL 4.50 for Vulkan, or shader model 3 HLSL as D3D9 ports bring it, compiled when the title builds. In
its `CMakeLists.txt`:

```
xsdk_add_shaders(title cube.vert cube.frag)
```

That runs `tools/build_shaders.py` into `title_shaders.h` in the build folder: each `.vert` and `.frag`, and each
`<name>_vs.hlsl` and `<name>_ps.hlsl`, through glslang to SPIR-V, the HLSL through glslang-xenon with the flags
xenos-toolchain/docs/xsc.md, "HLSL", gives (docs/GETTING_STARTED.md, section 4), then through xost's compiler at xsdk's
register split, optimised. The header takes the target's name, the first argument, and what's in it takes the files'
names. Once a target; a target's shaders, GLSL and HLSL, go in the one call. The header holds, for each file:
- `cube_vert`, an `XsdkBuiltShader`: its container, for `xsdk_shader_load`, its stage and its literals; `quad_ps.hlsl`
  gives `quad_ps_hlsl`;
- an enum of where its resources landed: `CUBE_VERT_PUSH`, the first float constant of its push constants, and
  `CUBE_VERT_PUSH_REGISTERS`, how many; `CUBE_VERT_UNIFORM_<set>_<binding>` and its `_REGISTERS` for a uniform block;
  `CUBE_FRAG_SAMPLER_<set>_<binding>`, a sampler's texture fetch constant;
- `xsdk_built_shader_set_literals`, which sets a shader's literals: the constants xost reads its numbers from, as
  Xenos has no immediates (xenos-toolchain/docs/xsc.md, "Literal constant pool").

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_shader_load(&shader, container, bytes)` | the microcode in GPU memory, in shared pages | `xsdk_shader_free` | drawing |
| `xsdk_shader_set_vertex_layout(&shader, elements, count, strides)` | the vertex shader's fetches patched for the layout | another layout; not while the shader may run | drawing |
| `xsdk_gpu_set_shaders(&vertex, &pixel)` | both shaders, for the draws after | the next set, or a resolve | drawing |
| `xsdk_built_shader_set_literals(&built)` | the shader's literals in its stage's constants | constants set over them | drawing |
| `xsdk_gpu_set_vertex_constants(first, floats, count)`, `xsdk_gpu_set_pixel_constants` | `count` float constants from `first`, 4 floats each, for the draws after | the next set of them; a resolve keeps them | drawing |
| `xsdk_gpu_set_vertices(stream, address, bytes)` | the vertex stream, for the draws after | the next set | drawing |
| `xsdk_gpu_draw(primitive, count)`, `xsdk_gpu_draw_indexed(primitive, address, count, indices_32_bit)` | the draw queued | | drawing |

- **Vertex inputs by location:** 0 is `XSDK_USAGE_POSITION`, 1 `XSDK_USAGE_COLOR`, 2 `XSDK_USAGE_TEXCOORD` index 0, 3
  `TEXCOORD` index 1 and so on; each `XsdkVertexElement` names its format, stream and offset
  (xenos-toolchain/docs/xsc.md, "Interface conventions"). Each location gives its usage alone, so locations can skip,
  0 and 2 without 1 (src/xsc_resolve.c, `resolve_input`). An input the shader never uses has no fetch, and its element
  is ignored.
- **Push constants and uniform blocks** take consecutive float constants, push constants first, then the blocks by set
  and binding: a member at byte `o` of a block at constant `b` is in `b + o / 16`, std140's layout. A `mat4` is
  column-major, a column a constant, so a C array of 16 floats, column after column, goes in as it is. Each stage has
  256 float constants of its own; the pixel shader's 252 to 255 are xsdk's.
- **Literals** sit after the blocks. Set a shader's after each `xsdk_gpu_set_shaders`, as another shader's literals may
  use the same constants.
- **Samplers** take texture fetch constants from 0 in each stage, by set and binding; a vertex shader that samples
  moves its own with `xsdk_shader_move_textures` so both stages fit.
- **HLSL's constants by name,** as a D3D9 port binds them (docs/scope/50-hlsl.md, step 6):
  `xsdk_shader_constant(&shader, "LightColor", XSDK_SHADER_FLOAT, &c)` (`shader.h`) reads the container's table and
  gives `c.index`, the first float constant, `c.count` and the rows, columns and elements. `register(cN)` is cN, and
  `register(sN)` fetch constant N, so `SetTexture(N)` is `xsdk_gpu_set_texture(N, ...)`. Find bools and ints by name
  and set each in every set the table names it in: a bool a pixel shader's if reads in execs of its own is a boolean
  constant, `XSDK_SHADER_BOOL`, bit `c.index % 32` of word `c.index / 32` for `xsdk_gpu_set_bool_constants`; an int a
  loop counts a loop constant, `XSDK_SHADER_INT`, set to `xsdk_gpu_loop_constant(n, 0, 1)`; either read as a value a
  float constant, 1.0 or 0.0 for a bool. A bool's `register(bN)` makes its boolean constant 128 + N, where
  `SetPixelShaderConstantB(N)` puts it, and an int's `register(iN)` its loop constant 16 + N in a pixel shader and N
  in a vertex shader; Khronos's glslang drops `b#` and `i#`, glslang-xenon keeps them.
- **Indices** are big-endian, as the CPU writes them, 16 bits each unless `indices_32_bit`, up to 65535 a draw.
- **Constants the GPU loads from memory,** `xsdk_gpu_load_vertex_constants` and `xsdk_gpu_load_pixel_constants`, go in
  whole groups of 4 from a multiple of 4, from 64-byte aligned memory; a group loaded isn't also set. The boolean and
  loop constants are set all together (`xsdk_gpu_set_bool_constants`, `xsdk_gpu_set_loop_constants`).
- **What the build takes:** xost's subset of GLSL (xenos-toolchain/docs/xsc.md; docs/PITFALLS.md, "xost's GLSL
  subset"), and of HLSL (xsc.md, "HLSL", names what it refuses). It stops with xost's reason for a shader it refuses,
  as in `bits.frag: xost didn't compile it (refused): v1 fragment: unsupported integer op BitwiseAnd`. It also stops
  for a shader needing a constant it doesn't write yet, a GLSL loop a uniform counts, a loop whose count is data, or a
  clip plane: give such a loop a constant count, which xost unrolls. An HLSL loop an int counts needs nothing more: the
  title sets the int.

`xsdk_xsc_compile` (`xsc.h`) is the same compiler in the title, for SPIR-V that arrives at run time; any thread may
call it. Instanced draws (`xsdk_gpu_draw_instanced`), depth, stencil, blending, culling, the viewport and the scissor
are in `gpu.h`, each a state computed once (`xsdk_gpu_depth_stencil_state` and the rest) and set for the draws after.

## Textures, render targets and resolves

[samples/cube](../samples/cube/main.c), [samples/hello](../samples/hello/main.c)

eDRAM holds what the GPU draws into: a colour target and its depth. Nothing reaches memory, the screen or a texture
until a resolve copies it out, and the resolve then clears eDRAM for the next frame.

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_texture_create(&texture, format, w, h, levels, tiled)` | the texture's layout and its memory | `xsdk_texture_free` | any |
| `xsdk_texture_upload(&texture, level, data, row_bytes)` | a level's texels copied in, in the CPU's byte order | another upload; invalidate it after | any, while the GPU doesn't read it |
| `xsdk_texture_sampler_state(&state, &sampler)` | nothing: the sampler's half of the fetch constant | | any |
| `xsdk_gpu_set_texture(slot, &texture, &state)` | texture fetch constant `slot` for the draws after | the next set; a resolve takes slot 0 | drawing |
| `xsdk_gpu_set_target(w, h)` | the colour target, its depth after it in eDRAM, a viewport over it; no depth test, blending, culling, scissor or clip planes | a resolve | drawing |
| `xsdk_gpu_set_depth_stencil(&state)` | the depth and stencil test, for the draws after | `xsdk_gpu_set_target`, a resolve | drawing |
| `xsdk_gpu_set_blend(&state)`, `xsdk_gpu_set_blends(states, count)` | blending and write masks, for the draws after; with `XsdkBlend.alpha_to_coverage`, target 0's alpha also drawing the share of each pixel's samples nearest it, a tie rounding up, so at 1 sample from alpha 1/2 | `xsdk_gpu_set_target`, a resolve | drawing |
| `xsdk_gpu_resolve(address, w, h, clear_argb)` | the colour target copied to `address`, tiled as the display reads it; eDRAM's colour cleared to `clear_argb` and its depth to far | | drawing |
| `xsdk_gpu_resolve_texture(&texture, clear_argb)` | the same into a tiled 32-bit RGBA texture of the target's size | | drawing |
| `xsdk_texture_download_rows(&texture, level, row, rows, data, row_bytes)` | rows copied out, untiled | | any, while the GPU doesn't write it |

- **After a resolve, set everything again:** the target, the depth test, the shaders and their literals, and slot 0's
  texture. Float constants stay. A draw after a resolve before `xsdk_gpu_set_target` stops at an assert.
- **What a frame shows is the clear before it:** a resolve copies eDRAM, then clears it, so the next frame's draws land
  on that clear. The first frame after `xsdk_gpu_open` lands on whatever eDRAM held.
- **A resolve's width is a multiple of 32.** Targets past eDRAM's 10 MiB, such as 1920x1080 with depth, go in tiled
  passes (`xsdk_gpu_begin_tiles`); 2 and 4 samples, several targets and float formats are `xsdk_gpu_set_target_samples`,
  `xsdk_gpu_set_targets` and `xsdk_gpu_set_targets_samples`, which takes 16_16_FLOAT, 16_16_16_16_FLOAT and 32_FLOAT at
  1 sample only.
- **Depth runs from 0, near, to 1, far,** and a resolve clears it to far unless `xsdk_gpu_set_depth_clear` says
  otherwise; `xsdk_gpu_set_resolve_clears` turns either clear off. Clip space is y up, as D3D's.
- **Texture coordinates** run as D3D's: v = 0 is the texture's first row, the one an upload writes first, so a quad
  whose top corners take v = 0 shows it at the top (tests/test_texture.c checks every texel lands so). A point-sampled
  `XSDK_FORMAT_A8R8G8B8` texel drawn without blending reaches the resolved frame exactly as uploaded.
- **Reading a frame back,** as the samples check themselves: the front buffer is laid out as a tiled
  `XSDK_FORMAT_A8R8G8B8` texture of the display's size, so `xsdk_texture_init` and `xsdk_texture_attach` over
  `xsdk_present_pixels(address)` give a texture to download rows from, once the frame's fence has passed. Its words are
  0xBBGGRRAA, B, G, R and A in memory (docs/HARDWARE.md, "A title's buffer on screen").

`texture.h` lists the 24 formats, mipmaps, cube and 3D textures and the sampler's filters and address modes.

## Recordings

[tests/test_replay.c](../tests/test_replay.c)

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_gpu_command_buffer_create(&buffer, dwords)` | GPU memory for the packets | `xsdk_gpu_command_buffer_free`, after a fence past its last run | drawing |
| `xsdk_gpu_record_begin(&buffer)` | GPU calls appending to the buffer instead of the ring | `xsdk_gpu_record_end`, which fails with `0xC0000023` if it outgrew the buffer | drawing |
| `xsdk_gpu_execute(&buffer)` | the recording queued, as often as asked | | drawing |

A recording holds draws and state, with no fences, resolves or executes; it saves the CPU writing the same packets
every frame.

## Threads and where they run

[samples/sound](../samples/sound/main.c), [samples/sdl3](../samples/sdl3/main.c)

The console has 6 hardware threads, two to each of 3 cores. `pthread_create`, `std::thread` and `xsdk_thread_create`
start a thread on its creator's hardware thread, so every thread a title starts shares the main thread's until placed
(docs/HARDWARE.md, "Threads on all six hardware threads, milestone 26").

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_thread_create_on(&handle, stack_bytes, entry, context, hardware_thread)` | a thread on that hardware thread, 1 MB of stack for 0 | its entry returning; `NtWaitForSingleObjectEx` on the handle waits for it, `NtClose` lets the handle go | any |
| `xsdk_thread_move(hardware_thread)` | the calling thread moved | another move | any |
| `xsdk_thread_sleep(ms)`, `nanosleep` | the thread asleep, about a millisecond longer than asked | | any |

- **Two busy threads on one core's two hardware threads** each get about half of it; on two cores, nearly all.
- **Priorities** are refused, `pthread_setschedparam` returning `ENOTSUP`.
- **Mutexes, condition variables, read-write locks, `once` and keys** are pthreads', and `std::mutex` and the rest on
  them. Free a mutex only once no thread can lock it again (docs/PITFALLS.md).
- **Atomics** cost by their order: a relaxed `__atomic_fetch_add` 55 ns, acquire or release 70, seq_cst 125, against
  16 ns for a plain increment (docs/HARDWARE.md, "Counted loops end in bdnz, milestone 52"). C++'s `operator++` on a
  `std::atomic` and the `__sync_*` calls are seq_cst, so ask for `memory_order_relaxed` where a counter orders nothing,
  and acquire and release for a flag that hands data over.
- **Callbacks the kernel makes** run on its threads: the audio render driver's on its audio thread, the vblank one,
  xsdk's own, in its interrupt handling. Keep them short.
- **`xsdk_thread_wait_counts`** counts yields and blocking waits, to tell sleeping from spinning.

## Files and paths

`game:\` is the folder the title was launched from, and the current directory when it starts. The C, POSIX and C++
calls take `/` or `\`:
- a relative path, `data/level1.bin`, is from the current directory, `chdir` changing it;
- a drive, `game:/data/level1.bin`, is that drive's;
- a path from `/` goes to the kernel as written, a device path such as `\Device\Harddisk0\Partition1\...`
  (src/path.c).

xsdk names the console's other drives, in any case: `hdd:` is the hard disk's data partition, `usb:` or `usb0:` the
first USB drive, `usb1:` and `usb2:` the others, `mu:` or `mu0:` and `mu1:` the memory units, and `dvd:` the disc. A
drive with nothing in it fails with `ENOENT` (docs/HARDWARE.md, "Drives, time zones and Xbox Home, milestone 54").

`open`, `read`, `write`, `pread`, `pwrite`, `lseek`, `fsync`, `stat`, `statvfs`, `opendir`, `mkdir`, `rename`,
`unlink`, `truncate`, `ftruncate`, stdio and `std::filesystem` all work there (tests/test_files.c,
tests/test_cpp_filesystem.cpp). `statvfs`'s block is the drive's allocation unit: 16 KB on the hard disk, 32 KB on the
USB stick tried. `truncate` and `ftruncate` write the zeros a file grows by themselves, as the hard disk brings back
what was there before. A file opens more than once, to read and to write, but one that's open can't be removed or
renamed, so `std::filesystem::remove_all` fails on a folder holding folders (docs/PITFALLS.md, TODO.md). Permissions
can't change, and there are no links. The log, `xsdk_log_write` from any thread, is `<title>.log` in `game:\`;
`stdout` and `stderr` go there too. With a `net_log.txt` beside the `.xex` each line also goes to a PC running
`tools/net_log.py`, and with a `dbgprint.txt`, or after `xsdk_log_set_dbgprint(true)`, to `DbgPrint`
(docs/GETTING_STARTED.md, "Reading the log").

## Sockets

[tests/test_net.c](../tests/test_net.c)

BSD sockets over XAM's: TCP and UDP over IPv4, `connect`, `bind`, `listen`, `accept`, `send`, `recv`, `sendto`,
`recvfrom`, `poll`, `select`, `fcntl`'s `O_NONBLOCK`, and `getaddrinfo` for names, through XAM's DNS, with numbers for
services. The network starts at the first socket. xsdk marks each socket insecure as it makes it, as only such
sockets reach the LAN or the internet (docs/HARDWARE.md, "Sockets reach the LAN and the internet when marked
insecure"). XAM refuses `SO_ERROR`, so a failed connection says only that it failed, `EIO`. TLS 1.2 is Mbed TLS's,
ported in mbedtls-xenon.

## Input and audio through the kernel

[samples/hello](../samples/hello/main.c), [samples/sound](../samples/sound/main.c), and SDL3's for both in
[samples/sdl3](../samples/sdl3/main.c)

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `XamInputGetState(0, 0, &state)` | controller 1's buttons, sticks and triggers in `state`; `0x48F` without one | | any |
| `XamInputSetState(0, 0, &vibration)` | the motors at those speeds | the next set; set them to 0 before ending | any |
| `XAudioRegisterRenderDriverClient(&client, &driver)` | the callback called for each frame the driver wants, about 188 a second | `XAudioUnregisterRenderDriverClient`; a call can still be running as it returns | any |
| `XAudioSubmitRenderDriverFrame(driver, frame)` | the frame queued for the driver | | the callback |

- **Controllers:** 0 to 3 are controllers 1 to 4. Sticks are signed 16 bits and read a little off centre at rest,
  triggers 0 to 255, buttons `INPUT_GAMEPAD_*` bits (docs/HARDWARE.md, "Runtime, milestone 2").
- **Audio:** a frame is 256 samples of each of 6 channels at 48 kHz, big-endian floats a channel after another, in
  5.1's order, front left first (docs/HARDWARE.md, "Runtime, milestone 2"), in memory from
  `MmAllocatePhysicalMemoryEx(0, bytes, PAGE_READWRITE, 0, UINT32_MAX, 0)`. That memory is cached, and frames written
  into it with plain stores play as written, with no flush (docs/HARDWARE.md, "Runtime, milestone 2"). The driver
  reads the frame it took last until it asks for the next, so a callback fills another. samples/sound keeps its frames
  to the end, as a call can outlast unregistering.
- **Submit a frame in every call,** a silent one when there's nothing to play, as SDL-xenon's driver does: what the
  driver plays after a call that submits none hasn't been measured.
- **Work samples out away from the callback.** No callback xsdk has run does floating point, and whether the kernel's
  audio thread may hasn't been checked. samples/sound copies a wave table's float bits as words, and SDL-xenon mixes on
  a thread of its own and hands the callback a finished frame.
- **SDL3** has both, as gamepads and audio streams, through SDL-xenon (docs/PARITY.md, "Libraries ported to xsdk").

## Saves, the user and XAM's dialogs

[tests/save_check.c](../tests/save_check.c), [tests/test_xam.c](../tests/test_xam.c), through `xam.h`

| Call | Leaves set | Undone by | Threads |
|---|---|---|---|
| `xsdk_xam_prepare(&overlapped)` | the overlapped zeroed, with an event of its own | `xsdk_xam_returned` if the call didn't pend, else `xsdk_xam_wait` once it's done | any |
| `xsdk_xam_returned(&overlapped, XamCall(..., &overlapped))` | `ERROR_IO_PENDING` while XAM runs the call, else its result | | any |
| `xsdk_xam_wait(&overlapped, timeout_ms, &length)` | `ERROR_IO_INCOMPLETE` until the call is done, then XAM's result and length | | any |
| `xsdk_save_mount(&save, user_index, "save", file_name, display_name)` | the package opened, or made, as `save:` once `xsdk_save_wait` says it's done | `xsdk_save_unmount` | any |
| `xsdk_save_unmount(&save)` | the package flushed then closed, once `xsdk_save_wait` says it's done; files still open in it fail | | any |
| `xsdk_xam_signed_in_user(&user_index, gamertag)` | the first signed-in user and their gamertag; `ERROR_NO_SUCH_USER` with nobody | | any |
| `xsdk_xam_keyboard(...)`, `xsdk_xam_message_box(...)` | the dialog open; its text or button lands once the call is done | | any, while the drawing thread presents |
| `xsdk_system_ui_open()` | whether the Guide or a dialog of XAM's is up | | one thread, once a frame |

- **Poll once a frame, with a timeout of 0.** The Guide and XAM's dialogs show only while the title keeps presenting
  (docs/HARDWARE.md, "The Guide shows through VdSwap"), so a wait that blocks the drawing thread on a dialog never
  ends. A timeout is for a thread that doesn't present.
- **Keep the overlapped and the `XsdkSave` alive** until the call is done: XAM writes into them as it finishes.
- **Saves with nobody signed in** belong to `XSDK_XAM_USER_NONE`; user 0's fail then
  (results/2026-10-19-milestone-51/probe_xam.log). Delete an unmounted one with
  `XamContentDelete(save.user_index, &save.data, &save.overlapped)`, prepared and waited for as above.
- **A title ID keeps saves apart:** packages sit in `Content\0000000000000000\<title ID>\00000001\` on the hard disk,
  a folder for each title ID, and every title without one of its own shares 0x5853444B's. Give it one, and the name
  the dashboard shows, with `xsdk_add_xex(target TITLE_ID 0x... NAME "...")`.
- **Pause for the Guide** while `xsdk_system_ui_open` says it's up; a dialog's own call says when it closes.

## The console's settings

[tests/test_system.c](../tests/test_system.c), through `system.h`

At start-up xsdk sets newlib's time zone, `TZ`, from XAM's `GetTimeZoneInformation`, so `localtime`, `mktime` and
`strftime`'s `%Z` give the console's local time, daylight saving included; a title's own `setenv("TZ", ...)` and
`tzset()` replace it. `xsdk_system_language()` is the console's language, `XGetLanguage`'s number, which
`XsdkLanguage` names, and `xsdk_system_country()` its country, XAM's number for it in its list of locales, 36 for the
US. A console set to US Eastern gives `TZ` as `<EST>5:00<EDT>4:00,M3.2.0/2:00:00,M11.1.0/2:00:00`; one with no time
zone set gives UTC (docs/HARDWARE.md, "Drives, time zones and Xbox Home, milestone 54").

## Porting: the C and POSIX calls

A C17 or C++23 program builds with newlib, libstdc++ and xsdk's POSIX layer. What links, by a link of every name below
against `libxsdk.a`, newlib and libstdc++ on 2026-10-19:

- **Has:** stdio, strings, math, `malloc` and `aligned_alloc`, `time`, `clock`, `strftime` and the rest of `time.h`, in
  the console's time zone, `setjmp`, `getenv` and `setenv`; `open`, `openat`, `close`, `read`, `pread`, `write`,
  `pwrite`, `lseek`, `fstat`, `fsync`, `fdatasync`, `ftruncate`, `fstatvfs`, `isatty`, `stat`, `statvfs`, `chdir`,
  `getcwd`, `mkdir`, `unlink`, `unlinkat`, `rename`, `truncate`, `readlink`, `pathconf`, `opendir`, `fdopendir`,
  `readdir`, `rewinddir`, `closedir`; `pthread_create`, `join`, `detach`, attributes and stack sizes, mutexes, condition
  variables, read-write locks, `once` and keys, `sched_yield`; `clock_gettime`, `gettimeofday`, `nanosleep`; `sysconf`,
  `getentropy`, `getpid`, `kill`, `raise`, `signal`, `abort`, `exit`, `atexit`; the sockets above, `getaddrinfo`,
  `inet_pton` and `inet_ntop`; C++23's threads, atomics, exceptions, `chrono`, `filesystem` and `format`
  (docs/PARITY.md).
- **Refuses at run time:** `pthread_setschedparam`, `ENOTSUP`; `pthread_cancel`, `link` and `symlink`, `ENOSYS`;
  `fchmod` and `fchmodat` changing a file's permissions, `ENOTSUP`; `fcntl` on a file, `ENOSYS`.
- **Doesn't link:** processes, `fork`, `vfork`, `execve`, `wait`, `waitpid`; memory maps, `mmap`, `munmap`, `mprotect`;
  `dlopen`; `dup`, `dup2`, `pipe`, `socketpair`; `rmdir` (`remove` and `unlink` remove an empty folder), `readv`,
  `writev`, `lstat`, `fstatat`, `access`, `faccessat`, `realpath`, `chmod`, `chown`, `fchown`, `umask`, `utime`,
  `utimes`, `utimensat`, `futimens`, `mkdirat`, `renameat`, `fpathconf`; `sleep` and `usleep` (`nanosleep` is there);
  `clock_getres`, `clock_nanosleep`, `times`, `alarm`; `sigaction`; `sem_init`, `sem_wait`, `sem_post`; `pthread_exit`,
  `pthread_kill`, `pthread_sigmask`, `pthread_atfork`, barriers and spin locks; C11's `thrd_create`; `sched_setaffinity`
  (`xsdk_thread_move` is there); `getuid`, `geteuid`, `getppid`, `getpagesize`; `gethostbyname` and `getnameinfo`
  (`getaddrinfo` is there); `getrandom` (`getentropy` is there); `posix_memalign` (`aligned_alloc` is there); `ioctl`.

A port supplies what it needs of the last list itself, on the calls above, or leaves it out. Shared libraries don't
exist: everything links into the one `.xex` (cmake/xsdk.cmake builds static libraries).
