# Milestone 73: Pokémon Colosseum built natively from its decomp

Proposed on 2026-11-03, at the user's request. Milestone 71 (the GameCube recompiler, for the same game) is paused.

## Why

Milestone 71 runs Colosseum's code through a recompiler on one core. On the console its battle menu holds about 30 VI
fields a second against 60, at about 28 host clocks a guest instruction, and a profiled run (2026-11-03,
local/m71/play_native_prof_20261103.log) shows no single block worth more than 5%.

The game now has a complete decomp, so its C can be compiled for Xenon directly:
- with no guest CPU to emulate and no checks;
- the game's threads, its graphics and its sound on separate cores;
- the code still big-endian 32-bit PowerPC, as the GameCube's was.

## What there is

- **The decomp:** github.com/dougchansan/pkmn-colosseum (MIT, cloned at Z:\pkmn-colosseum, ad02df4).
  - It's a matching decompilation of GC6E01's DOL and 25 REL modules (src/rel): 8,677 of 8,677 functions.
  - 133 functions are still compiled from assembly (counted with `#if 0` alternatives left out):
    - most of them are SDK and runtime code: PPCArch 18, the matrix library 16, OSCache 15, the C runtime 13, OSContext 11, MetroTRK 19;
    - the rest are game code: the game's own thread switch (src/game/gs_thread_exact_800F0030.c: saving and loading
      registers, `_threadSwitch`) and a few others.
  - The code includes HAL's sysdolphin (src/hsd: aobj, cobj, dobj, fobj, archive and so on), MusyX (src/musyx) and the
    Dolphin SDK.
- **Aurora** (aurora-xenon, on xsdk) replaces the Dolphin SDK for decomp ports:
  - modules in aurora-xenon/lib/dolphin: AR, card, dvd, gd, gx, ms, mtx, os, pad, si, thp and vi;
  - nod_iso, which reads files from the player's disc image.
  - Dusklight (Twilight Princess's decomp) runs on the console through it.
- **HAL's sysdolphin on Aurora:** the Melee port (Z:\360-ports, milestone 18) runs the same library through Aurora on
  xsdk.
- **From milestones 68 and 71:**
  - Colosseum's GX already draws through Aurora on the console.
  - The DSP's mixing, in VMX, already plays its sound.
- **A PC port:** git.softwareauralabs.com/chase/pokemon-colosseum, cloned at Z:\colosseum-pc-port.
  - It's built from the same decomp for x86-64, and has no licence, so it's read for reference only and nothing is
    copied from it.
  - It lists what a port has to replace (host/: os, dvd, rel, gsthread, card, pad, vi, audio, mtx, thp).
  - Its README says some call arguments needed filling where CodeWarrior accepted a call that a modern compiler
    rejects (tools/arity.py), so expect the same with GCC.

## Decisions for the user

1. **Where the port lives.** I'd make it a fork of the decomp, branch `xenon`, with the port in its own directory, and
   decomp files changed only as section 14 of CODE_GUIDELINES.md allows (marked, small, upstream style). Local until
   you say otherwise. The alternative is a separate repo that takes the decomp as a submodule plus patches.
2. **Threads.** I'd keep single-core rules for the game:
   - its own threads stay cooperative, switched by a PowerPC version of its thread switch, on one hardware thread;
   - Aurora's graphics and the DSP's mixing run on other cores, as milestone 71 split them.
   Running the game's threads truly in parallel would race in code written for one core.
3. **Movies.** I'd use the decomp's own THP code compiled natively and measure it, before reaching for Aurora's THP
   or milestone 72's decoder.

## The plan

1. **It compiles and links for xsdk.** The decomp's C goes through xsdk's GCC into one title:
   - every unit compiled, with no original object code;
   - the RELs linked statically, their prologs and epilogs called where OSLink would have run them;
   - CodeWarrior-only constructs fixed under fork rules: assembly functions as C, or as xsdk PowerPC assembly where they
     switch threads or contexts; calls with missing or extra arguments; pragmas; section attributes.

   Done when the title links, and the build lists every SDK function still stubbed.
2. **It boots.** Aurora's SDK replaces the Dolphin SDK:
   - files come from the player's disc image through nod_iso, as in Dusklight;
   - the game's low-memory OS globals and its few direct hardware-register accesses are mapped;
   - MusyX's DSP work goes to milestone 71's mixer;
   - controllers and the memory card go through Aurora.

   Done when Colosseum reaches its title screen on the console with sound.
3. **It plays.** Story mode from its start and Battle Now battles to their end, on the console. Done when the player's
   own play through those is right and at full speed: 60 fields a second, the game's 30 frames, in the battle menu too.
   Where it isn't, the console's numbers say why.

## Checks

- **The build** counts its compiled units, and fails when original object code or an unresolved stub is left in.
- **Pictures against the original code:** milestone 71's title runs the original code and draws through the same
  Aurora. At fixed points (the title screen, and a Battle Now battle driven by an input script from a fixed start), the
  native port's frames are compared with milestone 71's. A deliberate break, such as one changed game constant, fails
  the comparison.
- **On the console:** boot to the title, and the player's play. No timed gameplay runs.

## Not in it

- other games and regions, including XD (GC6E01 only);
- the Wii, and the GBA link (the multiboot image);
- enhancements: widescreen, higher internal resolution, 60 fps;
- a release, until the user decides.

## Size

Large: steps 1 and 2 are each a milestone's worth. Step 3 depends on what the first boot shows.
