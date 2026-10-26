# Milestone 67: Flycast in RetroArch, with a PowerPC SH-4 recompiler

Started on 2026-10-26 at the user's word. The user asked for LRPS2 with paraLLEl-GS; paraLLEl-GS draws in Vulkan
compute and Xenos has no compute stage (vulkan4xenon:docs/SUPPORT.md:43, 59), and LRPS2's recompilers have no
PowerPC backend, so the Dreamcast through Flycast's libretro core was taken instead. The defaults below are mine, to
be changed at the user's word; the work runs beside milestone 66's.

## Sources

- **Flycast** (flyinghead/flycast at tag `v2.7`, GPL-2.0), cited as `flycast:<path>:<line>`; a local clone,
  `Z:\cores-xenon\flycast`, branch `xenon`.
- **SwanStation's PowerPC emitter**, ours (swanstation-xenon:src/core/cpu_recompiler_emitter_ppc.cpp, .h).

## What there is

- Flycast builds a libretro core with CMake's `LIBRETRO` option (flycast:CMakeLists.txt:94). Its SH-4 recompilers are
  backends of its IR, shil, after an SSA pass and register allocation (core/hw/sh4/dyna/: shil, ssa, ssa_regalloc,
  ngen.h), for x86, x64, ARM and ARM64 (core/rec-*); there's an interpreter (core/hw/sh4/interpr/). The AICA's ARM7
  has an interpreter and recompilers of its own (core/hw/arm7/). The host CPU and recompiler are chosen in
  core/build.h.
- **No big-endian host:** nothing in Flycast's own code tests the host's byte order. The Dreamcast is little-endian,
  so every access to its memory, its PowerVR's display lists and textures, the GD-ROM and the AICA's sound memory
  assume the host is too.
- RetroArch on xsdk (milestone 65) links a core statically and draws through vulkan4xenon, Vulkan 1.0 without compute.

## Defaults

1. **Flycast as a libretro core** in RetroArch's xsdk build, as milestone 65's cores are.
2. **The recompiler is a PowerPC backend of shil** beside rec-ARM64's, on SwanStation's emitter; the interpreter first,
   the baseline its blocks are checked against. The ARM7 stays on its interpreter.
3. **Guest memory stays little-endian;** host accesses swap, byte-reversed loads and stores in the recompiler.
4. **Vulkan through vulkan4xenon,** without order-independent transparency, which needs fragment stores.

## Steps

1. **The core** (agent A): a static library for xsdk on the interpreter, its big-endian errors fixed and audited,
   its memory without mapped views, linked into RetroArch's `.xex`.
2. **The recompiler** (agent B): the PowerPC shil backend, with an encoding test and a console self-check against
   the interpreter.
3. **On the console** (me): homebrew the user picks, on the interpreter, then the recompiler.

## Not in it

Naomi and Atomiswave, the modem and broadband adapter, the ARM7 and DSP recompilers, and anything needing compute.

## Size

Very large: the big-endian audit of the whole core is a milestone's worth on its own.
