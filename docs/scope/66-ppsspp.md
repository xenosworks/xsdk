# Milestone 66: PPSSPP in RetroArch, with a PowerPC recompiler

Started on 2026-10-25 at the user's word, after milestone 65 brought RetroArch up on the console: PSP is the
recompiler milestone after it (65's decision 2), and the user asked for an agent on PPSSPP's recompiler at once, with
the usage left that night. The defaults below are mine, to be changed at the user's word.

## Sources

- **PPSSPP** (hrydgard/ppsspp at tag `v1.20.4`, GPL-2.0-or-later), cited as `ppsspp:<path>:<line>`; a local clone,
  `Z:\cores-xenon\ppsspp`, branch `xenon`.
- **SwanStation's PowerPC emitter**, ours (swanstation-xenon:src/core/cpu_recompiler_emitter_ppc.cpp, .h), and its
  code generator, which runs a recompiler on the console today.

## What there is

- PPSSPP has a libretro core in its tree (`libretro/`), and JITs for x86, ARM, ARM64 and RISC-V. Its newer ones are
  backends of its IR (`Core/MIPS/IR/`, the RISC-V and x64 IR-native backends); the IR interpreter runs any host.
- The PSP is little-endian; PPSSPP reads guest memory through types that swap on a big-endian host (`Common/Swap.h`,
  `u32_le` and the rest), from Dolphin, but no big-endian host has built it for years, so how much holds isn't known.
- RetroArch on xsdk (milestone 65) links a core statically, one `.xex` a core, and draws through vulkan4xenon, which
  compiles SPIR-V on the console the first time (vulkan4xenon:docs/SUPPORT.md, Shaders). Xenos has no compute.

## Defaults

1. **PPSSPP as a libretro core** in RetroArch's xsdk build, as milestone 65's cores are, not a standalone title.
2. **The recompiler is a PowerPC backend of PPSSPP's IR,** an IR-native JIT as the RISC-V one is, on SwanStation's
   emitter; the IR interpreter first, as the baseline each block's result is checked against.
3. **Guest memory stays little-endian;** every host access goes through the swapping types, fixed where it doesn't.
4. **Vulkan through vulkan4xenon,** with what needs compute off (texture upscaling on the GPU).

## Steps

1. **The core** (agent A): PPSSPP's libretro core as a static library for xsdk, its big-endian build errors and the
   accesses that miss the swapping types fixed, the IR interpreter as its CPU, linked into RetroArch's `.xex`.
2. **The recompiler** (agent B): the PowerPC IR-native backend, its emitter from SwanStation's, built for xsdk, with
   a self-check that runs IR blocks through both the backend and the interpreter on the console and compares them.
3. **On the console** (me): a homebrew PSP title the user picks, on the interpreter, then on the recompiler, its speed
   logged.

## Not in it

Ad hoc networking, the PSP's camera and microphone, texture upscaling, and anything needing compute.

## Size

Very large. Step 1's big-endian work and step 2's backend are each a milestone's worth; that night's agents start
both.
