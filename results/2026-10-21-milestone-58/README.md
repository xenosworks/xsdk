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
- [xsdk_test_hw1.log](xsdk_test_hw1.log): xsdk_test at `f7d3a9f`, its audio checks' feeders started on hardware
  thread 1. None ended within its 5 s join, the kernel took a frame or a few from each and then no more, and the
  breaks' runs, the polling one among them, failed the same; sdl_check and faudio_check, feeding the same `audio.c` from
  hardware thread 0, passed in the same session. The joins, not hardware thread 1, turned out the cause (below).
- [xsdk_test_join_timeout.log](xsdk_test_join_timeout.log): xsdk_test at `aac0931`, which logs the
  kernel's calls and the feeder's progress 100 ms into each check, with the main thread asleep, then at the feeder's
  join and after the close: each 5 s wait on a thread's handle timed out within 13 ms by the time base, the calls
  keeping their rate until the close.
