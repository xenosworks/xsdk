# Milestone 58, steps 1 and 2 on the console (2026-10-21)

Each log unchanged, from runs by the console agent; docs/HARDWARE.md, "Audio's feeding, milestone 58", says what they
found.

- [step1_dusk_m58.log](step1_dusk_m58.log): Dusklight's title scene, 32 windows of 10 s, built from xsdk `bdfd1d1`,
  SDL-xenon `d16871365` with [sdl_m58.patch](sdl_m58.patch) and Dusklight with [dusk_m58.patch](dusk_m58.patch), at 4
  samples. After each frame-rate line, `audio sdl:` and `audio dusk:` give SDL-xenon's driver's counters and Dusklight's
  render times for the window.
- [step2_probe_audio.log](step2_probe_audio.log): tests/probe_audio.c at `c7338fc`, every case but callback_wake.
- [step2_callback_wake_ntsetevent.log](step2_callback_wake_ntsetevent.log): callback_wake at `c7338fc`, the callback
  calling `NtSetEvent` on the title's handle, as the DPC flag was clear.
- [step2_callback_wake_kesetevent.log](step2_callback_wake_kesetevent.log): callback_wake at `dc127c7`, the callback
  calling `KeSetEvent` on the event's object. Its "registering" line still names the choice by the DPC flag, which
  `dc127c7` took out of the code but not out of the line.
