# Milestone 70: Mono on xsdk

Agreed on 2026-10-30. Mono's JIT runs C# on the console; the Silksong port brought it up (Z:\silksong-xenon, local),
and its hello, ABI and GC titles pass on the console. This milestone makes it a port xsdk users can take, like
SDL-xenon or curl-xenon.

## Decisions (the user, 2026-10-30)

1. **The fork's home:** xenosworks/mono-xenon, a fork of mono/mono, branch `xenon`.
2. **All seven of what remained**, below, are this milestone.

## Steps

1. **Mono's regression tests on the console.** `mono --regression` over `mono/mini`'s tests (Makefile.am's
   `regtests`), from a title, every optimisation set it runs; plus the hello, ABI and GC titles, and checks of threads,
   finalizers, timing and files through `game:\`. Done when every test passes or is skipped with a reason that holds
   for the console (SIMD intrinsics, for one, if the PPC backend has none).
2. **The known gaps:** GC pauses that read 0 ms; native varargs and the argument types `mini-ppc.c` stops on; file
   names over FATX's 42 characters, through Mono's assembly preload hook.
3. **The platform code:** the POSIX functions xsdk lacks go into xsdk, each with an xsdk_test check and a PARITY row;
   what Mono only needs to not fail becomes configure or `#ifdef` work in the fork; a static library can bring its own
   `.jit` pool to `xsdk_jit_pool`.
4. **A build from a fresh clone:** one script that configures and builds the runtime and the class libraries, with
   `make` working through (eglib's tests are what stop it now).
5. **History:** the fork's commits squashed by topic, `xenon` force-pushed once, before anyone builds on it.
6. **Guidelines and docs:** CODE_GUIDELINES.md on the fork's xsdk code as section 14 says; the fork's
   `docs/README-xsdk.md`; xsdk's README and PARITY rows.
7. **Measurements on the console:** memory a title pays for Mono, start-up to `Main`, and the JIT's speed on the
   regression tests.

## Rules

- Section 14 of CODE_GUIDELINES.md for the fork: Mono's style, our rules otherwise. Our own code (the title, the
  shims xsdk takes in) follows the guidelines in full.
- The regression tests and our checks run on the console; Xenia is not evidence here (CLAUDE.md).
- Nothing from the Silksong project's game data goes into either repository.
