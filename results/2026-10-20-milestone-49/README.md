# Milestone 49: the pass after a depth clear

docs/scope/49-hiz-clear-pass.md; docs/HARDWARE.md, "The pass after a depth clear, milestone 49". Run on the console
through the console agent on 2026-10-20, each probe case from a fresh boot.

## The probe

`tests/probe_hiz_pass.c` (`make hiz_pass_probes`) runs the case named in `game:\hizpass_start.txt`, with the pass xsdk
draws after a depth clear replaced through a hook built into its own copy of src/gpu_resolve.c. Each case logs
milestone 30's four sequences after a clear, xsdk_test's 12 frames of near and far rectangles (`test_hiz`), and the
time of 16 resolves clearing colour and depth against 16 clearing colour alone, on the GPU's clock. From the third
round each case also drew 64 rectangles of scattered sizes and places, then 16 whose first block lies in the top two
rows of blocks, each the first draw after its own clear.

| Case | Milestone 30's four | 12 frames | Scattered, 64 | Top rows, 16 | Over a colour-only resolve |
|---|---|---|---|---|---|
| `none`, no pass | 2 wrong: 199 and 22 pixels | 8 wrong | 64 wrong | 13 wrong | 0.04 to 0.07 us |
| `whole`, the whole target, as before | all right | all right | all right | all right | 29.4 to 29.5 us |
| `rows8`, the top 8 rows | all right | all right | all right | all right | 0.99 to 1.13 us |
| `rows16`, `rows32` | all right | all right | | | 1.3, 2.1 us |
| `quarter`, the top left quarter | all right | all right | | | 7.9 us |
| `column`, the left 32x720 | all right | all right | | | 1.4 us |
| `strip2` to `strip20`, 2 to 20 blocks at the top left | all right | all right | all right (`strip2`) | 1 wrong (`strip2`) | 0.67 to 0.82 us |
| `left1`, the top left block | 2 wrong: 71 and 22 | 8 wrong | | | 0.7 us |
| `corner`, the top right block | 2 wrong: 199 and 22 | 8 wrong | | | 0.5 us |
| `corner2`, the top right 2 blocks | 1 wrong: 199 | 1 wrong | | | 0.6 us |

Logs: `probe_<case>.log`; `none`, `whole` (run as `full`, xsdk's pass then), `rows8` and `strip2` from the last round,
the rest from the round that introduced them. The colour-only resolve took 269.9 to 270.0 us each time.

The event cases, `EVENT_WRITE` of type 4, 10 or 20 in place of the pass, weren't run. The top 8 rows already cost
about 1 us, so an event could save at most that, and `EVENT_WRITE_SHD` with event 20 wedged the GPU before
(docs/HARDWARE.md, "VdInitializeEngines without microcode").

## xsdk's pass over the top 8 rows

Built clean at `7736a51` (local/m49/build.sh), vulkan4xenon and Dusklight against it:
- **`probe_full.log`:** the probe's `full` case, xsdk's pass now: every check right, the 64 scattered and 16 top-row
  rectangles among them, for 1.08 us over a colour-only resolve.
- **`xsdk_test.log`:** 476 passed, 2 failed, the controller's checks with the pad gone idle.
- **`xsdk_test_nohizpass.log`,** the pass left out: the controller's two and
  `hiz: near and far drawn both ways over depth clears, less-than and greater-than, every pixel right` failed.
- **`vulkan4xenon_test_1.log`, `_2.log`:** 492 of 492 each.
- **`dusklight.log`:** the title scene at 29.7 to 30.0 fps.

## The milestone

Done on 2026-10-20: a resolve clearing depth costs 1.08 us more than one clearing colour alone, at most 5, and every
check after a clear holds. The pass is skipped where the buffer stays off.
