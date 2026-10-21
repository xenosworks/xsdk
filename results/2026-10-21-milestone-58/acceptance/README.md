# Milestone 58 on the console (2026-10-21)

Each title's log, unchanged, from runs by the console agent. [SHA256SUMS](SHA256SUMS) has their hashes.
docs/HARDWARE.md, "Milestone 58 on the console", says what they found.

- [xsdk_test](xsdk_test/xsdk_test.log) at `3dfe975`: 635 of 635. Then the six breaks of
  [cases.tsv](../../../tests/breaks/m58/cases.tsv), one folder each, each failing the checks its line names.
- [sdl_check](sdl_check/sdl_check.log), SDL-xenon `c105fb7bf`: 12 passed; the one failure is "a gamepad is connected",
  with no controller connected then. [sdl_late_wait](sdl_late_wait/sdl_check.log), its break, fails the no-silent-frame
  and 48 kHz checks.
- [faudio_check](faudio_check/faudio_check.log), FAudio-xenon `9ec024c`: 7 of 7, and its seven breaks, one folder each,
  each failing its own check, `samples_swapped` the resampling check too, its voice being 16-bit.
- [dusk](dusk/dusk_m58_api.log): Dusklight's title scene at 4 samples on xsdk's audio API through SDL-xenon
  `c105fb7bf`, its 32 windows each followed by xsdk's counters: 60,065 frames played, none silent.

sdl_check, faudio_check and Dusklight ran on xsdk at `f7d3a9f`; `src/audio.c` has changed since only in a comment.
