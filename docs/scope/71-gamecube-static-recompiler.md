# Milestone 71: GameCube games recompiled ahead of time, from the player's own disc

Agreed on 2026-11-02. The user asked to take milestone 68's GameCube machine (docs/scope/68-gamecube.md) from its JIT
to a static recompiler, for full speed, and chose its shape from the options here.

## Decisions (the user, 2026-11-02)

1. **Xenon code straight from the game's PowerPC,** not C. Gekko and Xenon share the instruction set and byte order,
   so a game's code, already compiled and register-allocated by its own compiler, translates nearly one for one, and
   guest registers can stay in host registers across calls; C would hand them over through memory at every function
   boundary, the cost that sank the PSP recompiler's whole functions (milestone 66).
2. **M68's JIT as the fallback** for code the tool didn't translate: indirect targets it couldn't resolve and code a
   game loads at run time.
3. **Pokémon Colosseum first,** the user's own disc, which reaches its title on M68's machine.
4. **One .xex per game, built from the player's ISO on their PC.** It holds the game's code, so it is never
   distributed, never in a repository and never in the store; the tool and the runtime are ours to publish.

## What there is (M68)

- **The translator** (gamecube-xenon:src/jit/translator.cpp, translate_*.cpp), on SwanStation's emitter: Gekko blocks
  into Xenon code, guest GPRs, LR and CTR in host registers by a fixed mapping (src/jit/jit_internal.h,
  `GUEST_HOST_REGISTER`), guest CR the host's but for field 2, XER and FPSCR the host's, guest FPRs cached per block
  in f14-f31; loads and stores rebased onto guest memory, the hardware's ranges through handlers; paired singles;
  interrupts taken at block exits by a downcount; idle loops found and skipped.
- **The block cache** (src/jit/jit.cpp): direct exits linked block to block, indirect ones (`blr`, `bctr`) through a
  two-level dispatcher table, blocks dropped when the code under them changes.
- **The machine:** guest memory and ARAM, the DSP's HLE, the disc, GX through Aurora's command processor, VI, pads,
  memory cards; the interpreter, and the console check of the JIT against it block by block.

## The plan

1. **Find the game's code, on the PC.** Read the DOL, and any REL modules, from the ISO; follow branches and calls
   from the entry point and the exception vectors, and code pointers in data (vtables, function tables, a REL's
   relocations), into a map of functions and blocks. Jump tables the analysis can't bound go in a per-game hints
   file. Done when Colosseum's map covers, against M68's JIT run of its title and first scene, every block the JIT
   translated there.
2. **Translate and link it, on the PC.** M68's translator, built for the PC and writing big-endian code words, over
   the whole map, into one image: direct branches straight to their targets, calls as host `bl` and returns as host
   `blr` (LR holding host addresses, opaque to the game, which only saves and restores it), indirect branches through a
   guest-to-host table with the JIT behind a miss. The machine's exception entry and `rfi` translate a guest pc they
   meet (a new thread's entry, a vector) through the same table. Done when Colosseum reaches its title on the
   console from the image, the JIT taking only what the map missed.
3. **Optimise across blocks.** What the JIT can't see block by block: guest FPRs kept in host registers through a
   function, CR and XER updates dropped where nothing reads them, loads and stores proven to reach RAM without the
   hardware check, downcount checks only on backward branches and calls. Each change measured on the console.
4. **Code that changes.** A store or DMA over translated code, or `icbi`, hands that range to the JIT from then on.
5. **The per-game .xex:** the machine, the image, its table and the JIT, built by one command from the ISO.

## Checks

- Every translated block against the interpreter on the console, as M68's JIT is checked, with deliberate breaks.
- Colosseum to its title and into play; the map's coverage and the JIT's share of the run in the log.

## Not in it

C output, scheduling for Xenon's in-order core (a later pass if the measurements call for it), the Wii, and games
other than Colosseum until it runs.

## Size

Large: step 1 and step 2 are each a milestone's worth; step 3 is open-ended.
