# What xsdk can do, and what it can't yet

xsdk's goal is to let anyone make homebrew and port games to the Xbox 360 without Microsoft's SDK. This page is the
map: for each area, what a title gets from xsdk today, how to use it, which test proves it on the console, and what's
missing. It was drawn up on 2026-10-15, at xsdk `3e889a7`, brought up to date on 2026-10-20, at `e43c0e1`, after
milestones 41 and 44 to 52, and again for milestone 54's files, drives, settings and Xbox Home. SCOPE.md has the
milestones and TODO.md the open items; where they and this page disagree, they win, and this page is out of date.

**Status:**
- **Yes:** works, and a test proves it on the console.
- **Partial:** some of it works, or only through raw kernel calls.
- **No:** not there yet.
- **Out:** won't be in xsdk (end of the page).
- **Pending:** implemented and built, with console verification still required.

**What retail titles call** is judged from Xenia (xenia-project/xenia `95a5c3e`, in `xenos-toolchain/reference/xenia`),
which implements what the games it runs need:
- **The kernel:** its table lists 922 xboxkrnl exports. 239 have code in Xenia, 200 of them more than a stub. xsdk
  imports 60 (`imports/imports.txt`).
- **XAM:** its table lists 1,735 exports. 169 have code in Xenia, 106 of them more than a stub. xsdk imports 50, 22 of
  them for sockets.

## Building and packaging

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Toolchain: GCC 15, binutils, newlib for `powerpc-xsdk-elf`; counted loops end in `bdnz` | Yes | `tools/toolchain/build.sh`, in WSL; glslang-xenon for shaders (docs/GETTING_STARTED.md, section 4) | every build; `bdnz`, `tests/probe_ctr.c` (docs/HARDWARE.md, "Counted loops end in bdnz, milestone 52") | A prebuilt toolchain |
| A `.xex` the retail loader takes, imports by ordinal | Yes | `tools/elf2xex.py`, `src/xsdk.ld`; a title ID and name of its own through `--title-id` and `--title-name`, or `xsdk_add_xex(target TITLE_ID id NAME "...")` | `tests/xex_checks.py`; the name, `make cmake_check` | Compression; an icon; signing, so it runs on RGH and JTAG consoles only. The loader finds the name, but the dashboard's storage view lists the title as an unknown game and XAM leaves the name out of a save's header (docs/HARDWARE.md, milestone 51) |
| Make and CMake builds, shaders included | Yes | `Makefile`; `cmake/xsdk.cmake`, its `xsdk_add_xex()` and `xsdk_add_shaders()`, which builds GLSL and HLSL in one call | `make cmake_check` (`tests/cmake/`), its HLSL drawn on the console | Shared libraries: static only |
| The boot logo before the title's first frame | Yes | `xsdk_splash_show` (`splash.h`) | xsdk_test | |

## C and C++ runtime

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| C17 on newlib: stdio, strings, math, `malloc` | Yes | newlib | xsdk_test | |
| C++23: exceptions, RTTI, threads, atomics, chrono, filesystem, format | Yes | libstdc++ | `tests/test_cpp_*.cpp` | Names in `std::stacktrace`, which gives addresses only; native TLS, as `thread_local` is emulated (TODO.md) |
| `memcpy` and `memmove` on VMX | Yes | `src/copy.c` | `tests/test_copy.c` | |
| Loop unrolling | Partial | GCC's -O2 default; `-funroll-loops` per file | `tests/probe_unroll.c` | Not on for everything: it made Dusklight bigger and no faster (docs/HARDWARE.md, "Loop unrolling, milestone 35") |

## Memory

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| A heap of all the title's free memory | Yes | `sbrk` (`src/sbrk.c`) | `tests/test_memory.c` | |
| GPU memory, write-combined and physical | Yes | `xsdk_gpu_alloc` (`gpu.h`) | xsdk_test | Small allocations: each takes a 64 KiB page, and only shaders share (TODO.md) |
| Returning the heap top; anonymous mappings and page protection | Pending | `sbrk` decommits 64 KB chunks; `mmap`, partial `munmap`, `mprotect` | M55 raw allocation/protection probe; `test_pages.c` console pending; host bookkeeping checks pass | File/shared/fixed mappings, runtime execute protection |
| Executable image pool and explicit cache flush | Pending | `.jit`, `xsdk_jit_pool`, `xsdk_code_flush` | `probe_exec` returned 42 then 99; SDK API `test_pages.c` console pending | Builtin/libgcc clear-cache patch and rebuild: [TODO.md](../TODO.md) |
| Virtual region queries | Yes | `NtQueryVirtualMemory(address, information, 0)` for `NtAllocateVirtualMemory`/`mmap` regions | [Console query log](../results/2026-10-20-milestone-55/probe_query_memory.log): committed, reserved, free | Image pages return C000000D; use `MmQueryAddressProtect` for image protection |
| Physical page protection | Yes | `MmSetAddressProtect`, `MmQueryAddressProtect` | M55 probe | |
| Memory statistics | Partial | `MmQueryStatistics` (`kernel.h`), raw | `tests/test_memory.c` | Xenia's statistics are estimates |

## Threads and synchronisation

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Threads, joins, detached threads, stack sizes | Yes | pthreads (`src/pthread.c`), `std::thread` | `tests/test_pthread.c`, `test_threads.c` | |
| Placing threads on the 6 hardware threads | Partial | `xsdk_thread_create_on`, `xsdk_thread_move`; new `xsdk_thread_place` and `xsdk_pthread_handle` | `test_placement.c`; raw remote placement M55 probe; new wrappers and C++ handles pending | An unplaced thread runs on its creator's hardware thread |
| Mutexes, condition variables, read-write locks, once, keys | Yes | pthreads | `tests/test_pthread.c` | A mutex holds a pointer to a critical section `malloc`ed on first lock: locking one after its memory is freed corrupts the heap (TODO.md) |
| Priorities | Pending | `xsdk_thread_set_priority`, pthread scheduling calls; -16..16, default 0 | Raw kernel samples and +2 share: M55 probe; wrappers/all levels: `test_thread_control.c`, console pending | POSIX scheduling policies are not separate kernel policies |
| Semaphores | Pending | Raw kernel calls and unnamed process-local POSIX `sem_*` | `test_semaphore.c`, console pending; concurrent host mocks pass | Named/shared semaphores |
| Waiting on several objects; clearing and pulsing events | Yes | Raw waits on up to 64 objects, signal-and-wait, `NtClearEvent`, `NtPulseEvent` | M55 probe; expanded `test_objects.c` pending | |
| Owned mutexes, waitable timers, lock-free lists | Yes | Raw mutant, timer and SLIST exports in `kernel.h` | M55 probe; expanded and concurrent `test_objects.c` pending | Timer APC routines |
| APCs, alertable waits, I/O completion ports | No | | | Left out of M55 |

## Time

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Wall clock, monotonic clocks, sleeps | Yes | `clock_gettime`, `gettimeofday`, `nanosleep` (`src/time.c`), `std::chrono` | `tests/test_clocks.c`, `test_cpp_chrono.cpp` | Short sleeps: a 1 ms sleep took about 2 ms (docs/HARDWARE.md, "Sleeps, write speeds, and a set-up that wouldn't run from a cached stream") |
| Local time in the console's time zone | Yes | `localtime`, `mktime`, `strftime`, newlib's `TZ` set at start-up from XAM's `GetTimeZoneInformation` (`src/system.c`) | `tests/test_system.c`, with US Eastern set and with no zone (docs/HARDWARE.md, "Drives, time zones and Xbox Home, milestone 54") | |

## Files, drives and storage

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Files and folders beside the `.xex` (`game:`) | Yes | POSIX `open`, `read`, `stat`, `opendir`, `mkdir`, `rename`, `unlink`; `std::filesystem` | `tests/test_files.c`, `test_cpp_filesystem.cpp` | Asynchronous I/O; a file that's open can't be removed or renamed, so `remove_all` fails on nested folders (TODO.md) |
| Other drives: the hard disk, USB, memory units, the DVD | Yes | `hdd:`, `usb:` to `usb2:`, `mu:` to `mu1:` and `dvd:`, each its device's root (`src/path.c`); a path starting with `\` goes to the kernel as written | `tests/test_files.c`'s drive checks: `hdd:` and a USB stick listed, `dvd:` and `mu:` empty; the device paths, `tests/probe_drives.c` (docs/HARDWARE.md, "Drives, time zones and Xbox Home, milestone 54") | A memory unit and a disc listed, with none on hand yet |
| Writing at an offset, flushing, free space | Yes | `pwrite`, `fsync`, `fdatasync`, `statvfs`, `fstatvfs` (`src/file.c`, `src/path.c`) | `tests/test_files.c`, a write past the end leaving zeros in the gap, as xsdk writes them | |
| Setting a file's size | Yes | `truncate`, `ftruncate`, writing the zeros a file grows by themselves, as the hard disk brings its old bytes back (docs/HARDWARE.md) | `tests/test_files.c` | |
| The DVD drive's tray | No | | | `HalOpenCloseODDTray`, which Xenia doesn't implement |

## Save data, content and profiles

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| XAM's asynchronous calls, which the rows below take | Partial | `xsdk_xam_prepare`, `xsdk_xam_returned` and `xsdk_xam_wait` (`xam.h`): poll once a frame, or wait on the call's event | `tests/test_xam.c`; how the calls complete, `tests/probe_xam.c` (results/2026-10-19-milestone-51/) | Completion routines, which arrive as APCs and need alertable waits |
| Save games in content packages, on the HDD or a memory unit | Partial | `xsdk_save_mount`, `xsdk_save_unmount` and `xsdk_save_wait` (`xam.h`), on the hard disk, a package a drive such as `save:`; `XamContentDelete` and `XamContentCreateEnumerator` raw | `tests/test_xam.c` with no profile; `tests/save_check.c` with one, read back after a reboot, two title IDs apart | Memory units and USB drives |
| Choosing a storage device; its state and free space | Partial | `xsdk_save_device`, the hard disk; `XamContentCreateDeviceEnumerator`, `XamContentGetDeviceState` and `XamContentGetDeviceData` raw | `tests/test_xam.c` | `XamShowDeviceSelectorUI` (xam_ui.cc), not imported |
| Sign-in state, profile ID, gamertag | Partial | `xsdk_xam_signed_in_user`, the first user signed in and their gamertag; `XamUserGetXUID` and `XamUserGetSigninInfo` raw | `tests/test_xam.c`, signed in or not; `tests/save_check.c`; `XamUserGetXUID`'s masks, `tests/probe_xam.c` (docs/HARDWARE.md) | |
| Profile settings: invert Y, difficulty, a title's own data | Partial | `XamUserReadProfileSettings` (`kernel.h`), raw | `tests/probe_xam.c`'s `settings` case read both | Writing them, `XamUserWriteProfileSettings` (xam_user.cc) |
| Achievements | No | | | Through `XMsgStartIORequest` (xam_msg.cc); low priority |

## System UI and the Guide

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| The Guide over the title | Yes | automatic, through `xsdk_present_*` | `tests/guide_check.c`, by hand | The Guide on the kernel's display mode and interlaced outputs (TODO.md) |
| Knowing when to pause for the Guide | Partial | `xsdk_system_ui_open` (`xam.h`), from the XN_SYS_UI notifications `tests/guide_check.c` reads; `XamIsUIActive` raw | `tests/save_check.c`, by hand | |
| Ending the title by itself while the Guide is open | No | | | It hangs as it ends; leaving through Xbox Home is fine (TODO.md) |
| On-screen keyboard, message boxes, sign-in prompt | Partial | `xsdk_xam_keyboard` and `xsdk_xam_message_box` (`xam.h`), each started in one call and polled once a frame; `XamShowSigninUI` raw | `tests/save_check.c`, by hand; `tests/probe_xam.c` | What a cancelled keyboard gives. XAM refuses a dialog with 5 under the Guide or straight after another, and needs an overlapped (docs/HARDWARE.md) |
| Keeping the screen from dimming in cutscenes | No | | | `XamResetInactivity` (xam_input.cc) |
| Where notifications pop up | No | | | `XNotifyPositionUI` (xam_notify.cc) |

## Input

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Controllers: buttons, sticks, triggers, rumble | Partial | `XamInputGetState` and `XamInputSetState` (`kernel.h`), raw; SDL3's gamepads | `tests/test_input.c`; SDL-xenon's `test/xsdk/sdl_check.c` | An xsdk call |
| Controller types: wheel, guitar, arcade stick | No | | | `XamInputGetCapabilities` (xam_input.cc) |
| Chatpad and USB keyboard | No | | | `XamInputGetKeystrokeEx` (xam_input.cc) |
| Headset, microphone, camera, Kinect | No | | | Xenia has stubs only (xam_voice.cc) |

## Audio

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Playing PCM: 6 channels of float at 48 kHz | Partial | the kernel's render driver (`XAudioRegisterRenderDriverClient`, `kernel.h`), raw; SDL3's audio | `tests/test_audio.c`; `tests/audio_check.c`, by ear | An xsdk call; a mixer; recording |
| XMA decoding in hardware | No | | | `XMACreateContext` and the rest of xboxkrnl_audio_xma.cc |
| Speaker configuration; the dashboard's volumes | No | | | `XAudioGetSpeakerConfig`, `XAudioGetVoiceCategoryVolume` (xboxkrnl_audio.cc) |

## Video output

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| 1280x720, scaled to the output up to 1080p | Yes | `xsdk_display_open`, `xsdk_present_*` (`display.h`, `present.h`) | `tests/test_present.c`, `test_scale.c` | |
| 1280x720 letterboxed on a 4:3 screen | Yes | `xsdk_present_*`: 1280x960 front buffers, the frame on rows 128 to 847 between black bars, the title still drawing 1280x720 | `tests/test_scale.c`, forced on a widescreen screen; a 4:3 480p capture (docs/HARDWARE.md, "Letterboxing, milestone 54 step 5") | |
| 2 or 3 front buffers, paced to vblank, swap intervals | Yes | `xsdk_present_open` | `tests/test_pacing.c` | |
| Other modes: 1080p, 480p and the rest | No | `XSDK_DISPLAY_KERNEL` keeps the kernel's own | | |
| 720p on interlaced outputs, 480i and 1080i | No | the kernel's 640x480 there | | TODO.md |

## GPU

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Draws, indexed or not: points, lines, line strips, triangle lists, strips and fans; up to 16 vertex streams (`XSDK_VERTEX_STREAMS`); 18 vertex formats, their components picked in any order; instanced, elements at the instance's rate, a list's instances in pieces of up to 65,535 vertices a packet, indexed from repeated indices, a strip's or a fan's a packet each | Yes | `gpu.h`, `shader.h` | `tests/test_triangle.c`, `test_indexed.c`, `test_vertex.c`, `test_instancing.c` | More than 65,535 indices a draw; primitive restart (TODO.md); 10-bit vertex formats |
| Shader constants, set by the CPU or loaded by the GPU from memory | Yes | `gpu.h` | `tests/test_constants.c` | |
| Depth, stencil, two-sided too, blending, write masks, culling, depth bias, scissor, viewport, user clip planes, point sizes from a vertex shader, wireframe and point fill | Yes | `gpu.h` | `tests/test_depth.c`, `test_raster.c` | |
| Hierarchical depth | Yes | automatic after a depth clear, at 1, 2 and 4 samples, on targets up to 1280 wide and 1024 rows times samples; the pass that sets it up covers 8 rows, about 1 us where the whole target took 29 | `tests/test_hiz.c`; at 2 and 4 samples `test_tiles.c` and `tests/probe_msaa.c` (results/2026-10-20-milestone-44-step-3); the pass, `tests/probe_hiz_pass.c` (results/2026-10-20-milestone-49) | Larger targets, untried |
| Textures: 24 formats with DXT1, 3 and 5, DXN and DXT5A; signed and gamma reads; linear and tiled; mips; anisotropy; swizzles | Yes | `texture.h` | `tests/test_texture.c`, `test_formats.c`, `test_anisotropy.c` | 32-bit floats in two and four channels, signed DXN and DXT5A (TODO.md); 128-bit textures (SCOPE.md); LOD clamps; mipped linear textures (TODO.md) |
| Textures sampled in vertex shaders too, both stages' among the 32 fetch constants | Yes | xost (`xenos-toolchain/docs/xsc.md`, "Vertex textures"); `xsdk_shader_move_textures` (`shader.h`) | `tests/test_texture.c` moves a fetch; vulkan4xenon_test draws through it, on the console (vulkan4xenon milestone 22) | |
| Mirror-once, halfway and border addressing, a black or white border | Yes | `XSDK_ADDRESS_*` and `XsdkSampler`'s `border` (`texture.h`) | `tests/test_sampling.c` | The YCbCr borders (TODO.md) |
| Cube textures with mips, and 3D textures of one level, tiled | Yes | `xsdk_texture_init_cube`, `xsdk_texture_init_3d` and the slice uploads (`texture.h`) | `tests/test_cube_volume.c` | Linear ones; a 3D texture's mips; resolving into a face or slice (TODO.md) |
| Render targets in eDRAM, 1, 2 or 4 samples; past eDRAM in tiled passes at any of them | Yes | `xsdk_gpu_set_target*`; `xsdk_gpu_begin_tiles` | `tests/test_render.c`, `test_multisample.c`, `test_tiles.c` | The 16- and 32-bit float formats at 2 and 4 samples, whose resolves don't average on Xenos (results/2026-10-20-milestone-44-step-3) |
| Float, 10-bit and 64-bit targets: 2_10_10_10, 7e3, 16_16_16_16, 16- and 32-bit floats, at 1 sample; all but the 16- and 32-bit floats at 2 and 4 | Yes | `xsdk_gpu_set_targets`, `xsdk_gpu_set_targets_samples`, `xsdk_gpu_resolve_target` | `tests/test_targets.c`, `test_multisample.c` | 16_16, 32_32_FLOAT and gamma targets (TODO.md); blending into the 16- and 32-bit floats, which Xenos doesn't (docs/HARDWARE.md); the 16- and 32-bit floats at 2 and 4 samples, whose resolves don't average (results/2026-10-20-milestone-44-step-3) |
| Four targets at once (MRT), each blended and masked on its own, at 1, 2 or 4 samples | Yes | `xsdk_gpu_set_targets`, `xsdk_gpu_set_targets_samples`, `xsdk_gpu_set_blends`; xost's outputs at locations 1 to 3 | `tests/test_targets.c`, `test_multisample.c`; xost's `tests/test_color_outputs.py` | The 16- and 32-bit floats among them at 2 and 4 samples |
| Alpha to coverage at 1, 2 or 4 samples: target 0's alpha draws the share of each pixel's samples nearest it, every pixel alike | Yes | `XsdkBlend.alpha_to_coverage`, through `xsdk_gpu_set_blend` or `xsdk_gpu_set_blends` | `tests/test_multisample.c`; `tests/probe_msaa.c` (results/2026-10-20-milestone-44-step-4) | Dithered coverage, each pixel of a quad its own thresholds, as libxenon's offsets 3, 1, 0, 2 give it |
| Resolves of colour and depth, to memory and textures, with a clear | Yes | `xsdk_gpu_resolve*` | `tests/test_render.c`, `test_depth.c` | Clearing eDRAM without a copy (SCOPE.md) |
| Resolving part of a target into the same texels | Partial | `xsdk_gpu_resolve_texture_rect` (`gpu.h`) | `tests/test_resolve_rect.c` | Into another place in the texture; part of the depth; rectangles off the 8-pixel grid (TODO.md) |
| Command buffers recorded once and run many times | Yes | `xsdk_gpu_record_*`, `xsdk_gpu_execute` | `tests/test_replay.c` | Fences and resolves inside a recording; recording on several threads, as the ring takes one at a time |
| Occlusion queries, as D3D's visibility query, and GPU timers | Partial | `xsdk_gpu_begin_visibility`, `xsdk_gpu_end_visibility`; `xsdk_gpu_write_clock` (`gpu.h`) | `tests/test_visibility.c`; `test_gpu.c` | Sample counts: `ZPASS_DONE` hangs the GPU, so a query answers whether its draws rasterized anything, a hidden one too (TODO.md) |
| Memory export from shaders | No | | | xost doesn't emit it (SCOPE.md) |

## Shaders

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| GLSL to Xenos microcode, offline | Partial | glslang to SPIR-V, then xost (`xenos-toolchain/`), `xsdk_add_shaders` | xost's tests; xsdk_test's grids | A subset of GLSL (`xenos-toolchain/docs/xsc.md`): `if` on data as selects of both arms, on a uniform in execs on the predicate; loops unrolled, or hardware loops where an int in a block counts them, a `break` or `continue` on data gated; early returns, but not in a loop; a `discard` anywhere but a hardware loop; `switch` as an if/else chain, no fall-through; no bitwise operations or shifts, which Xenos lacks; `gl_ClipDistance` only as `dot(gl_Position, a plane from a block)`, no `gl_CullDistance` |
| SPIR-V to microcode on the console | Yes | `xsdk_xsc_compile` (`xsc.h`) | `tests/test_xsc.c` | Compiling GLSL on the console (docs/scope/shader-compiling.md) |
| Shader model 3 HLSL to Xenos microcode, offline | Yes | glslang-xenon to SPIR-V, then xost, `xsdk_add_shaders` (`xenos-toolchain/docs/xsc.md`, "HLSL"); D3D9's semantics, VPOS and VFACE; `s#`, `c#`, `b#` at boolean constant 128 + N and `i#` at loop constant 16 + N in a pixel shader, set by register or by name through the constant table (`xsdk_shader_constant`); HLSL's truncating `fmod`; `if`, early returns, `switch`, `clip` and `discard` anywhere; skinning from int indices | xost's `tests/test_hlsl_*.py`, 157 shaders of a corpus and 21 idioms against their GLSL twins (results/2026-10-20-milestone-50); xsdk_test's HLSL grid (`tests/test_hlsl.c`), each cell broken on purpose once; `tests/cmake/cmake_check.cpp` | The refusals xsc.md's "HLSL" names: a `discard` in a hardware loop, a `switch` case falling through or left from inside an if, two members in one register; shader model 4 and later, assembly shaders and Effects files |

## Networking

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| TCP and UDP sockets, `poll`, `select`, non-blocking | Yes | BSD sockets (`src/socket.c`) | `tests/test_net.c`, with `tools/net_echo.py` on the PC | IPv6; why a connect failed, as XAM refuses `SO_ERROR` (src/socket.c) |
| DNS | Yes | `getaddrinfo` (`src/netdb.c`) | `tests/test_net.c` | |
| TLS 1.2 | Yes | the mbedtls-xenon fork | docs/HARDWARE.md, "TLS 1.2 through Mbed TLS" | TLS 1.3; an HTTP client |
| System Link | No | | | XNet's secure transport (xam_net.cc) |

## Libraries ported to xsdk

Each in its own repository under xenosworks.

| Library | Status | What it gives | Verified by | Missing |
|---|---|---|---|---|
| SDL3 (SDL-xenon) | Partial | audio, up to four controllers, paths, threads, timers | `test/xsdk/sdl_check.c` there | A video, render or GPU driver: titles draw with xsdk's GPU layer or vulkan4xenon (SDL-xenon's docs/README-xsdk.md) |
| Vulkan 1.0, a subset (vulkan4xenon) | Partial | a Vulkan layer on xsdk's GPU layer | its test title and vkbench | What its docs/SUPPORT.md leaves out |
| Mbed TLS 3.6 (mbedtls-xenon) | Yes | TLS 1.2 and its cryptography | its self-tests and HTTPS check | TLS 1.3 |

## System information and control

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| Ending the title, back to the dashboard | Yes | `exit` (`src/exit.c`) | every title | |
| Random numbers | Yes | `getentropy` (`src/entropy.c`) | mbedtls-xenon's checks | |
| Language, country, time zone | Yes | `xsdk_system_language`, `xsdk_system_country` (`system.h`); the time zone as newlib's, above | `tests/test_system.c` | |
| Region and AV settings | No | | | `ExGetXConfigSetting` (xboxkrnl_xconfig.cc) raw |
| A routine when Xbox Home ends the title | Yes | `xsdk_on_terminate` (`terminate.h`), on a thread of the console's with the title frozen, its handles closed and `game:` gone | `tests/terminate_check.c`: the routine's marker read after, and the console carrying on; the notification, `tests/probe_terminate.c` | Flushing stdio and running `atexit` there, which the frozen title's handles and locks rule out (docs/HARDWARE.md) |
| Launching another `.xex`, and passing it data | No | | | `XamLoaderLaunchTitle`, `XamLoaderSetLaunchData` (xam_info.cc) |
| Rebooting and powering off | No | | | `HalReturnToFirmware` (xboxkrnl_hal.cc) |
| The SMC: LEDs, temperatures, fan, power button, AV cable | No | | | `HalSendSMCMessage` and `HalGetCurrentAVPack`, which Xenia doesn't implement, so only the console can settle them |
| Hashes and encryption for a title's own data | No | | | `XeCryptSha`, `XeCryptAesCbc` (xboxkrnl_crypt.cc), `RtlComputeCrc32` (xboxkrnl_rtl.cc) |

## Debugging and profiling

| Capability | Status | How | Verified by | Missing |
|---|---|---|---|---|
| A log file per title, readable while it runs; `stdout` and `stderr` into it | Yes | `xsdk_log_*` (`log.h`) | `tests/test_files.c` | |
| The log over the network, as the title writes it | Yes | `net_log.txt` beside the `.xex`, `tools/net_log.py` on the PC | results/2026-10-20-milestone-53 | A reliable stream: UDP drops what the network drops, and says where |
| `DbgPrint` | Yes | `kernel.h`; the log's lines too with `dbgprint.txt` or `xsdk_log_set_dbgprint` | | |
| Crash reports with function names | No | | | Milestone 40, paused after its probe (SCOPE.md) |
| Counters: the ring, presents, thread waits | Yes | `xsdk_gpu_stats`, `xsdk_present_stats`, `xsdk_thread_wait_counts` | xsdk_test | A CPU profiler; a GPU capture tool |

## Documentation and samples

| Capability | Status | Where | Missing |
|---|---|---|---|
| How the SDK was built, and every fact the console settled | Yes | SCOPE.md, docs/HARDWARE.md | It's a lab notebook, not a guide |
| Getting started: toolchain to a title running on the console | Yes | docs/GETTING_STARTED.md | |
| An API guide: call order, lifetimes, what undoes what | Yes | docs/GUIDE.md, a task at a time, with the headers' comments | |
| Pitfalls: big-endian data, stores to GPU memory, the Xenia differences | Yes | docs/PITFALLS.md | |
| Samples to start from | Yes | `samples/`: hello, triangle, cube, sound, sdl3 and a template, each checking itself (`make samples`) | |

## Out of xsdk

- **Xbox Live and anything online through Microsoft:** signing in to Live, matchmaking, presence, friends, parties, the
  marketplace, Live's secure transport. xsdk stays clear of anything that could serve online modding.
- **The hypervisor:** no HV or HVX calls, and nothing that reaches the console's keys (CODE_GUIDELINES.md 1.2).

## The order proposed for the gaps, and where each stands

What a porter would hit first, as proposed on 2026-10-15, each a milestone agreed when it starts:

1. **Catching faults:** a report of every CPU exception in the log, with its address, registers and stack, and the
   title ended back at the dashboard (SCOPE.md, milestone 40). Open: paused after its probe.
2. **Docs and samples,** which make everything after them easier to use, for people and for the AI assistants most of
   them will work through. Done, milestone 41.
3. **The log over the network,** so a title's failure can be seen without copying files back. Done, milestone 53.
4. **XAM's asynchronous calls, then save data and drives.** XAM's calls and saves on the hard disk done, milestone 51;
   `hdd:`, `usb:`, `mu:` and `dvd:`, milestone 54. Open: saves on memory units and USB drives, the device selector.
5. **Profiles and system UI.** The signed-in user and gamertag, the keyboard, message boxes and pausing for the Guide
   done, milestones 37 and 51. Open: writing profile settings, ending the title under the Guide, and what a cancelled
   keyboard gives.
6. **Threading for Windows-style code:** waits on several objects, event resets, priorities, timers, semaphores. Open.
7. **Drawing for SDL3 ports:** a render driver, as SDL3 has none on the console. Open.
8. **The GPU's missing features.** Float and 64-bit targets, MRT, predicated tiling, multisampling with alpha to
   coverage, cube and 3D textures, instancing, and queries whether draws rasterized done, milestones 44 and 47 among
   them. Open: sample counts in queries, memory export, and the formats and textures in the GPU table's Missing column.
9. **Audio:** XMA decoding, speaker configuration. Open.
10. **Input and the system:** keyboards, controller types, language and region, launching titles, the SMC. Language,
    country and the time zone, milestone 54. Open: the rest.

Done since, beside the list: shader model 3 HLSL (milestone 50), loops in shaders (46), xost's code quality (45, 48),
the pass after a depth clear (49), and counted loops in compiled CPU code (52).
