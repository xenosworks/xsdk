<div align="center">
  <img src="assets/xsdk.png" alt="xsdk" width="640">
</div>

# xsdk

A from-scratch homebrew SDK for the Xbox 360. It builds `.xex` titles that run on the retail kernel and dashboard of
an RGH console, and its GPU layer drives Xenos directly, writing the command processor's packets itself. Everything
in it comes from public reverse engineering, mainly Xenia, libxenon and xost, and from results on the console: no
Microsoft tooling and no leaked SDK material.

**New to xsdk?** Start with [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md): from a Windows or Linux PC and an RGH
or JTAG console to a title of your own running on it. Then [docs/GUIDE.md](docs/GUIDE.md) has the calls by task,
[docs/PITFALLS.md](docs/PITFALLS.md) what breaks a title on the console and not on a PC, and `samples/` titles to
start from.

**Status:** every milestone in SCOPE.md, section 4, up to 58 has passed on the console as of 2026-10-21, but for 40,
catching faults, paused after its probe, and 42, whose render target formats came with vulkan4xenon's milestone 25
instead. [docs/PARITY.md](docs/PARITY.md) maps what a title gets today and what's missing, docs/HARDWARE.md records
each fact the console settled, and TODO.md what's still open.

## What's in it

- **Titles:** `tools/elf2xex.py` packs an ELF into a XEX2 the retail loader accepts, with imports by ordinal, its pages
  of zeros left out of the file, and a title ID, name and version of its own. The C17 runtime is on newlib, and the C++23 runtime has exceptions, threads, atomics,
  chrono, filesystem and format.
- **The system:** threads placed on the six hardware threads, with priorities; the kernel's waits on several objects,
  events, owned mutexes, timers and lock-free lists; files on `game:`, the hard disk, USB, memory units and the DVD;
  `mmap` and `mprotect`; the console's time zone and language; controllers; saves, profiles, the on-screen keyboard
  and message boxes through XAM; the Guide over the title and Xbox Home; launching another `.xex` with data
  (`thread.h`, `kernel.h`, `system.h`, `xam.h`, `terminate.h`).
- **Audio:** `xsdk_audio_*` feeds the kernel's render driver 6 channels at 48 kHz from a thread of the title's own
  (`audio.h`).
- **Networking:** sockets, DNS, the link and the console's address (`net.h`).
- **The GPU layer** (`include/xsdk/gpu.h`):
  - commands recorded into a stream the ring runs as indirect buffers, so the GPU works while the CPU records;
  - colour and depth targets in eDRAM at 1, 2 or 4 samples, up to four at once, in float, 10-bit and 64-bit formats
    too, and past eDRAM in tiled passes; hierarchical depth; resolves to memory and to textures;
  - shaders from xost, GLSL and Shader Model 3 HLSL compiled on the PC or SPIR-V on the console (`xsc.h`), with
    branches and loops, loaded with `IM_LOAD`, and their vertex layouts;
  - textures in linear and tiled layouts, 24 formats with DXT among them, mipmaps, cube and 3D textures, anisotropy
    and swizzles;
  - indexed and instanced draws of points, lines, triangle lists, strips and fans, up to 16 vertex streams, depth and
    two-sided stencil, blending, culling, clip planes and viewports;
  - shader constants set from the CPU or loaded by the GPU straight from memory;
  - command buffers recorded once and run many times, queries of whether draws rasterized anything, and GPU time
    stamps;
  - presenting paced to vblank at 720p, scaled to the output and letterboxed on 4:3, text in a bitmap font
    (`xsdk_text_*`), and the vulkan4xenon boot logo (`xsdk_splash_*`).
- **Debugging:** a log per title, readable while the title runs, and sent over the network to `tools/net_log.py` on
  the PC when a `net_log.txt` is beside the `.xex`.
- **Speed, on the console:** a cube with its own transform and tint takes 0.29 us to draw and submit, and `memcpy`
  copies 5.6 GB/s into write-combined memory (vulkan4xenon's vkbench; docs/HARDWARE.md, "The cost of a draw,
  milestone 38").

## Building

docs/GETTING_STARTED.md takes it a step at a time. The build runs on Linux: Debian, on its own or in WSL on Windows.
macOS hasn't been tried (docs/GETTING_STARTED.md, "macOS"). In short: the toolchain is GCC 15, binutils and newlib
for `powerpc-xsdk-elf`, built by `tools/toolchain/build.sh` (its specs and patches say what differs from a stock
build) and installed at `~/xsdk-gcc`. Then, in the repository:

```
make
```

That makes `build/libxsdk.a` and the test title, `build/xsdk_test.xex`, as well as the bring-up probes. A title links
its own objects and `libxsdk.a` with `src/xsdk.ld`, which starts it at `xsdk_start` in the library, then packs the ELF
with `tools/elf2xex.py`; vulkan4xenon's Makefile does it that way. CMake projects get the same through
`-DCMAKE_TOOLCHAIN_FILE=cmake/xsdk.cmake` and `xsdk_add_xex(<target>)`, and compile their GLSL and HLSL shaders with
`xsdk_add_shaders(<target> <files>)`; `make samples` builds the samples that way, and `make cmake_check` a C++ title.

## Testing

`build/xsdk_test.xex` runs every test in one title and writes a pass or fail line for each to `game:\xsdk_test.log`:
each title's log is named after its `.xex`. Copy it to the console, launch it from the dashboard, and read the log.
Xenia Canary serves for quick crash and smoke checks, but the console decides; CLAUDE.md lists where Xenia differs.

## Layout

- `include/xsdk/` and `src/`: the SDK; `imports/` the kernel and XAM imports it links by ordinal.
- `samples/`: titles to learn from and start from, each a CMake project; `samples/template` is a new title's start.
- `tests/`: the test title and the probes, titles that each answer one question on the console.
- `tools/`: the XEX packer, the shader and import generators, and the toolchain build.
- `SCOPE.md`: what the SDK covers and its milestones. `CODE_GUIDELINES.md`: how its code is written.
- `docs/GETTING_STARTED.md`, `docs/GUIDE.md` and `docs/PITFALLS.md`: how to make a title with xsdk.
- `docs/HARDWARE.md` and `results/`: what each console run showed, with its logs and captures.

## Related

- [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon), a Vulkan 1.0 subset built on xsdk's GPU layer, which
  Dusklight draws through.
- xost, the shader toolchain, is in `xenos-toolchain/`. The archived
  [xenos-toolchain](https://github.com/xenosworks/xenos-toolchain) repository holds it as it was before it joined xsdk
  on 2026-10-14.
- Libraries ported to xsdk, each a fork whose notes are in its `docs/README-xsdk.md`:
  [SDL-xenon](https://github.com/xenosworks/SDL-xenon) (SDL 3),
  [FAudio-xenon](https://github.com/xenosworks/FAudio-xenon) (XAudio2's API through FAudio),
  [imgui-xenon](https://github.com/xenosworks/imgui-xenon) (Dear ImGui),
  [curl-xenon](https://github.com/xenosworks/curl-xenon) (libcurl, HTTP and HTTPS),
  [mbedtls-xenon](https://github.com/xenosworks/mbedtls-xenon) (Mbed TLS, TLS 1.2) and
  [glslang-xenon](https://github.com/xenosworks/glslang-xenon) (glslang, its notes in `README-xsdk.md`).
- [aurora-xenon](https://github.com/xenosworks/aurora-xenon), encounter's Aurora with a vulkan4xenon backend, and
  [dusklight-xenon](https://github.com/xenosworks/dusklight-xenon), Dusklight on the Xbox 360 through it.

## Licence

xsdk is under the MIT licence (LICENSE), copyright Mena. A few files carry code from libxenon and Xenia under the
3-clause BSD licence; THIRD_PARTY_NOTICES.md has their notices and what a game built with xsdk ships to meet them. The
toolchain's patches are under the licences of GCC, binutils and newlib.

## Trademarks

xsdk isn't affiliated with or endorsed by Microsoft. Xbox and Xbox 360 are trademarks of Microsoft. The repository
holds no Microsoft code, binaries, firmware, keys or microcode (CODE_GUIDELINES.md).
