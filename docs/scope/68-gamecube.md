# Milestone 68: GameCube code on Xenon, through a PowerPC recompiler and Aurora's FIFO processor

Agreed on 2026-10-26. The user asked whether the GameCube's Gekko, a PowerPC like Xenon, could run through a
translation layer, as Aurora runs decompiled GameCube games but on the fly, from the original binaries, and whether
the FIFO worker would come from Aurora.

## Decisions (the user, 2026-10-26)

1. **A standalone title,** not a RetroArch core.
2. **devkitPPC and libogc installed** (devkitPro's `gamecube-dev`) to build the test programs here.
3. **Agents on steps 1 and 3 at once;** step 2 follows step 1.

## Sources

- **aurora-xenon** (our fork of encounter/aurora, MIT), cited as `aurora:<path>:<line>`; Z:\aurora-xenon, branch
  `xenon`, which Dusklight draws through on the console (vulkan4xenon).
- **Dolphin** (dolphin-emu/dolphin, GPL-2.0-or-later), cited as `dolphin:<path>:<line>`: the hardware's behaviour and
  pieces to adapt, its interpreter and its DSP HLE among them.
- **Xenia** for how Xenon executes (xenos-toolchain/reference, as in SCOPE.md); SwanStation's PowerPC emitter, ours.

## What there is

- **The same CPU family, the same byte order.** Gekko and Xenon are both big-endian PowerPC with 32-bit addresses in
  use, so most integer, branch and floating-point instructions translate one for one, and no data is swapped. What
  differs:
  - **Paired singles:** Gekko's `ps_*` keep two floats in a register and `psq_l`/`psq_st` load and store them
    quantized through the GQRs; Xenon has neither, so they go to VMX128 or FPU pairs.
  - **Xenon's registers are 64 bits:** a record form sets CR0 from the whole register, and carries come from 64 bits
    (xenia:src/xenia/cpu/ppc/ppc_emit_alu.cc:48-60), so 32-bit overflow, carry and sign must be kept right.
  - **The memory map:** GameCube code runs at 0x80000000 (cached) and 0xC0000000 (uncached) and reaches the
    hardware at 0xCC000000; a title's own image sits at 0x82000000. Every load and store is rebased onto guest memory,
    and the hardware's ranges go to its emulation.
  - **Supervisor code:** MSR, BATs, HID registers, the locked cache, exceptions and `rfi` run in user mode here, so
    they're emulated.
- **Aurora decodes the GPU's own command words.** Its GX API writes FIFO commands that a command processor on the
  render worker decodes: BP, CP and XF register writes, draws, indexed loads and display lists
  (aurora:lib/gx/command_processor.cpp, regs.cpp, dl.cpp), the raw XF viewport and projection writes included
  (aurora:lib/gx/regs.cpp:858-911). Where the hardware takes a physical address, Aurora takes host pointers through
  commands of its own, `GX_AURORA_*` (aurora:lib/gx/command_processor.cpp:872-1040): array bases, texture objects,
  TLUTs, EFB copies, display-list calls and frame boundaries.
- **Dolphin is the hardware's reference,** but needs a 64-bit host and has only x86-64 and ARM64 JITs, so it isn't
  ported whole; its pieces are adapted.

## The plan

A standalone xsdk title, as Dusklight is: Aurora owns the display, which a libretro core couldn't (RetroArch owns
it there).
1. **The machine on an interpreter.** Guest memory (24 MB, the 16 MB ARAM), Dolphin's Gekko interpreter adapted to it,
   the processor and memory interfaces, the timers and interrupts, VI enough to show the XFB, and a high-level boot
   that loads a DOL (no IPL dump needed). Done when a libogc program's console text shows on the screen.
2. **GX through Aurora.** The adapter: the write-gather pipe at 0xCC008000 feeding Aurora's FIFO ring, and the
   hardware's address-carrying commands rewritten into Aurora's (the table in the conversation of 2026-10-26: CP array
   bases, BP texture images and TLUT loads, EFB copies, `GX_CMD_CALL_DL`, the XFB copy and VI as frame boundaries), EFB
   copies landing in guest memory when the game reads them. Done when a libogc GX sample draws as Dolphin draws it on
   the PC.
3. **The recompiler.** PowerPC to PowerPC blocks on SwanStation's emitter: guest registers cached in host ones,
   rebased loads and stores with a masked base, the hardware's ranges through handlers, paired singles on VMX128,
   CR0 and carries kept 32-bit, interrupts at block ends, code invalidation on `icbi` and DMA. Checked block by block
   against step 1's interpreter, with deliberate breaks.
4. **For games.** The DSP through Dolphin's HLE of its common microcodes, audio streaming, pads, memory cards and the
   disc drive with ISO images.
5. **On the console:** homebrew first, then a game the user owns.

## Checks

- libogc samples and Dolphin's hardware tests (dolphin-emu/hwtests), built with devkitPPC, their output compared with
  Dolphin's on the PC; no commercial software in the repositories.
- The recompiler against the interpreter, block by block, on the console.

## Not in it

The Wii (its IOS, Bluetooth, NAND and the rest), Dolphin's GCZ and RVZ images, netplay, and the GameCube's GBA link.

## Size

Very large, several milestones' worth: step 1 and step 3 each, step 2 smaller for Aurora's part.
