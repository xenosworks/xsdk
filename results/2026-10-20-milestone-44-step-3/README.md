# Milestone 44 step 3: multisampled formats and targets

docs/scope/44-tiling-and-msaa.md, step 3. The probe ran on the console through the console agent on 2026-10-20, built
from `288fbaa` (`make msaa_probes`): its default group once, then `pattern_2`, `pattern_4`, `depth_2` and `depth_4`
each alone from a fresh boot, as they write `PA_SC_AA_MASK` values and copy sample selects xsdk never had. Logs:
`probe_msaa_<case>.log`, copied from `local/m44p2`. The build on top of it is not yet run on the console; the list to
run is at the end.

## What the probe found

Each format and four-target case cleared a 256x128 target with a resolve, drew a triangle and two quads, one of them
added onto itself, resolved into a texture of the format the target converts to, and read every texel back by value.

- **Four formats take samples** (`probe_msaa_default.log`, `formats_2`, `formats_4`). 8_8_8_8, 2_10_10_10, its 7e3
  float and 16_16_16_16 drew every pixel off the edges right at 2 and 4 samples, blended a quad onto itself as at 1
  sample, doubling it, and resolved every edge pixel at a step of the samples' average within the format's precision:
  543, 164 and 342 pixels at 0, 1 and 2 of 2 samples, and 492, 85, 97, 89 and 286 at 0 to 4 of 4, the same for each
  format; at 1 sample, 625 and 424.
- **The 16- and 32-bit floats don't average.** 16_16_FLOAT, 16_16_16_16_FLOAT and 32_FLOAT drew every pixel off the
  edges right and blended as at 1 sample (not at all), but every edge pixel between the steps resolved to a value
  between them: a half-covered 16_16_16_16_FLOAT pixel 0.625 1.0625 27.625 -98304 where 0.875 1.375 500.375 -0.5 is
  the average, 16_16_FLOAT 0.625 23.625 where 0.875 500.25 is, 32_FLOAT 17.6445 for 617.375. xsdk refuses the three at
  2 and 4 samples.
- **Tiles.** Each format's target kept a neighbour placed right after its counted tiles untouched and overwrote one at
  half the count, at each count: 32, 64 and 128 tiles at 1, 2 and 4 samples for the 32-bit formats, 64, 128 and 256 for
  16_16_16_16 and 16_16_16_16_FLOAT ("takes N eDRAM tiles" lines).
- **Four targets at once** (`four_2`, `four_4`): 8_8_8_8, 2_10_10_10, 16_16_16_16_FLOAT and 32_FLOAT at tiles 0, 64,
  128 and 256 at 2 samples and 0, 128, 256 and 512 at 4. The two non-float targets resolved by value with every edge
  step as alone; the two float ones between the steps as alone.
- **Rates** (`rates`, by the GPU's clock, 1280x256 targets): opaque and blended fills alike ran 3444 Mpixels/s at 1
  sample, 3435 at 2 and 3368 at 4, 6.90, 6.88 and 6.75 pixels a clock, so 6.90, 13.76 and 27.00 samples a clock. A
  1280x720 resolve clearing colour took 269.95 us whole at 1 sample and 270.12 at 2, and 276.51, 276.69 and 277.14 us
  in 3 bands of 256, 256 and 208 rows at 1, 2 and 4.
- **Hierarchical depth at 2 and 4 samples** (`hiz_1`, `hiz_2`, `hiz_4`, 1280x256 targets, xsdk's buffer, its pitch
  and its pass after a depth clear, let on by a hook): 16 hidden full-screen quads through the long shader took 84,988
  clocks with the buffer on and 7,145,088 to 7,156,276 off, 84.0 times as fast at 2 samples and 84.2 at 4 (83.9 at 1),
  less-than over a clear to 1 and greater-than over a clear to 0, every visible pixel drawn. Milestone 30's sequences
  after a clear, milestone 49's 64 scattered rectangles and its 16 starting in the top rows were all right at both
  counts.
- **The sample pattern** (`probe_msaa_pattern_2.log`, `probe_msaa_pattern_4.log`). Copy sample selects 0 to 3 each
  copied one sample, and each `PA_SC_AA_MASK` bit, set in every nibble, let one sample through, bit i to select i. In
  16ths of a pixel across, then down, from the pixel's centre:

  | Samples | 0 | 1 | 2 | 3 |
  |---|---|---|---|---|
  | 2 | (-4, 4) | (4, -4) | | |
  | 4 | (-2, -2) | (2, 2) | (-6, 6) | (6, -6) |

  Together they are milestone 32's places. At 2 samples sample 0 is the lower one, where Xenia takes it to be the
  upper (xenia:src/xenia/gpu/draw_util.h:315-320).
- **Depth resolves** (`probe_msaa_depth_2.log`, `probe_msaa_depth_4.log`): a shape at depth 0.25 over a clear to 1,
  resolved under each select into a D24S8 texture. Selects 0 to 3 each copied one sample's depth, at the places above.
  The averaging selects averaged depth: 4 gave 0xA07F7F where one of a pixel's two samples was covered, 5 the same for
  samples 2 and 3, and 6 0x703F3F, 0xA07F7F and 0xCFBFBF where three, two and one of four were, near their averages of
  about 0x700000, 0xA00000 and 0xD00000. Xenia copies one sample under them (xenia:src/xenia/gpu/draw_util.cc:717-757,
  `SanitizeCopySampleSelect`), so its three checks differed.

The default group logged 133 PASS and 10 DIFFERS, the 10 the float formats' resolves at 2 and 4 samples; each case
ended with `xsdk_gpu_close` 0.

## What's built

- **`xsdk_gpu_set_targets_samples(width, height, samples, targets, count, depth_tile)`** is `xsdk_gpu_set_targets`
  with a count, which passes 1. A 16_16_FLOAT, 16_16_16_16_FLOAT or 32_FLOAT target at 2 or 4 samples stops at an
  assert, and gpu.h says so beside the formats.
- **`xsdk_gpu_color_target_sample_tiles(width, height, format, samples)`** counts a target's tiles at a count, a 64-bit
  one twice a 32-bit one's. `xsdk_gpu_color_target_tiles` keeps its three parameters and passes 1, as
  `xsdk_gpu_target_tiles` and `xsdk_gpu_target_sample_tiles` were split at milestone 32, so vulkan4xenon builds
  unchanged.
- **Depth resolves copy sample 0** at 2 and 4 samples (copy sample select 0), one sample's depth as the scope says;
  colour resolves average as before.
- **The sample pattern** is in gpu.h's comment on `xsdk_gpu_set_target_samples`, a line a count.
- **Tiled passes at 2 and 4 samples:** `XsdkTiling.samples` takes 1, 2 or 4, the formats as above. A band's depth and
  colour are counted, placed and moved at the count, so the bands are as few as fit at it; a pass with a refused
  format or another count can't be tiled (`xsdk_gpu_tiling_bands` gives 0, `xsdk_gpu_begin_tiles`
  `STATUS_INVALID_PARAMETER`).
- **Hierarchical depth at 2 and 4 samples,** within what was tried: up to 1280 wide, as before, and 1024 rows times
  samples, so 1280x1024 at 1 sample, 512 rows at 2 and the probe's 256 at 4 (src/gpu_draw.c, `xsdk_gpu_hiz_fits`). The
  probe's hook that let it on goes; its target hook now runs through xsdk's code past the refusal, and its tile count
  is xsdk's.

## xsdk_test's new checks

In `tests/test_multisample.c`, at 240x128, milestone 32's triangle drawn through `xsdk_gpu_set_targets_samples` with
the probe's clears and colours, each texel read back by value, edge pixels at a half or quarter step within the
format's precision, every step seen:
- `multisample: 8_8_8_8, 16_16_16_16, 2_10_10_10 and its 7e3 float take their tiles at 1, 2 and 4 samples, a 64-bit
  target twice a 32-bit one's`
- `multisample: <format> at <n> samples: edge pixels blend in halves|quarters, the rest as worked out`, for 8_8_8_8,
  16_16_16_16, 2_10_10_10 and 2_10_10_10_FLOAT at 2 and 4 samples, 8 checks.
- `multisample: four at once at <n> samples, target <i>, <format>: edge pixels blend in halves|quarters, the rest as
  worked out`, targets 0 to 3 the same four formats, 16_16_16_16 second so a target follows it, at 2 and 4, 8 checks.

In `tests/test_tiles.c`, each frame shown in its own colours, every row of the tiled frame compared with an untiled
reference drawn in pieces that fit, moved by the viewport, the band boundaries among them:
- `tiles: 1280x720 at 2 samples in 2 bands of 384 rows, every pixel as untiled`, against 1280x512 pieces.
- `tiles: 1280x720 at 2 samples with depth whole in 3 bands of 256 rows, every pixel as untiled`, against the same.
- `tiles: 1280x720 at 4 samples in 3 bands of 256 rows, every pixel as untiled`, against 1280x256 pieces.

The bands are as few as fit, hierarchical depth on in the bands at 2 samples' 384 rows and 4 samples' 256 and in the
references' pieces. A piece's `gl_FragCoord` starts at its own top, so these frames take a flat tint where step 2's
take xost's gradient. With depth whole at 2 samples its colour doesn't fit beside it, so a target over it, its colour
on the depth's tiles, clears depth alone. `tiles: band counts as the tiles say, and passes that can't be tiled
refused` now expects 2 bands for 1280x720 with depth at 2 samples, 3 with depth whole there and 3 at 4 samples, and
refuses 16_16_16_16_FLOAT at 2 samples and 3 samples. Step 2's 1 sample frames' references go through the same code, a
target set by `xsdk_gpu_set_targets_samples` and a resolve by value, with the same layout and clear as before.

## The breaks

`sh tests/breaks/build.sh` in WSL builds `build/breaks/xsdk_test_<break>.xex` from `tests/breaks/<break>.patch`, with
milestone 51's two. Each must fail these checks, and the controller's when the pad is idle, and pass the rest:
- **`xsdk_test_msaa_sample0.xex`,** every resolve copying sample 0: `multisample: 2 samples: ...`, `4 samples: ...`,
  `2 samples, placed: ...` and `4 samples, placed: ...`, milestone 32's four; the 8 `multisample: <format> at <n>
  samples` checks; the 8 `multisample: four at once ...` checks. The tiled frames pass, the reference copying sample 0
  as well.
- **`xsdk_test_msaa_onesample.xex`,** `xsdk_gpu_set_targets_samples` setting 1 sample: the 8 `multisample: <format>
  at <n> samples` checks; the 8 `multisample: four at once ...` checks; the three `tiles: 1280x720 at <n> samples ...`
  frames, whose references it draws. Milestone 32's four pass, through `xsdk_gpu_set_target_samples`.
- **`xsdk_test_msaa_tiles64.xex`,** a 64-bit target's tiles not doubled at 4 samples: `multisample: 8_8_8_8,
  16_16_16_16, 2_10_10_10 and its 7e3 float take their tiles ...`, `multisample: four at once at 4 samples, target 1,
  16_16_16_16: ...` and `... target 2, 2_10_10_10: ...`, which it overlaps; targets 0 and 3 pass.
- **`xsdk_test_msaa_bands2.xex`,** a 4-sample pass's bands sized as for 2 samples, 2 of 384 rows, its colour past
  eDRAM's end with the assert that stops that dropped: `tiles: band counts as the tiles say, and passes that can't be
  tiled refused` (2 bands for 3) and `tiles: 1280x720 at 4 samples in 3 bands of 256 rows, every pixel as untiled`.
  The band's colour wraps past tile 2047 onto its depth; if that wedges the GPU instead, every check after it fails
  too.

## For docs/HARDWARE.md

Proposed section:

> ## Multisampled formats and targets, milestone 44 step 3 (2026-10-20)
>
> `tests/probe_msaa.c` on the console, its default group once and its pattern and depth cases each alone from a fresh
> boot (docs/scope/44-tiling-and-msaa.md, step 3). Logs: `results/2026-10-20-milestone-44-step-3/`.
> - **8_8_8_8, 2_10_10_10, its 7e3 float and 16_16_16_16 take 2 and 4 samples:** drawn, blended as at 1 sample, and
>   resolved with every edge pixel at a step of the samples' average. **16_16_FLOAT, 16_16_16_16_FLOAT and 32_FLOAT
>   don't average:** drawn right off the edges, their edge pixels resolved between the steps, a half-covered
>   16_16_16_16_FLOAT pixel 0.625 1.0625 27.625 -98304 for 0.875 1.375 500.375 -0.5. xsdk refuses those three at 2 and
>   4 samples (`xsdk_gpu_set_targets_samples`).
> - **Tiles:** each format's held at its count, a 64-bit target's twice a 32-bit one's, and four targets at once
>   resolved the non-float ones by value.
> - **The sample pattern,** in 16ths of a pixel across, then down, from the pixel's centre: at 2 samples 0 at (-4, 4)
>   and 1 at (4, -4); at 4 samples 0 at (-2, -2), 1 at (2, 2), 2 at (-6, 6) and 3 at (6, -6). Copy sample selects 0 to
>   3 each copy one sample, and `PA_SC_AA_MASK` bit i, set in every nibble, lets sample i through. At 2 samples sample 0
>   is the lower one, where Xenia takes it to be the upper (xenia:src/xenia/gpu/draw_util.h:315-320).
> - **Depth resolves:** selects 0 to 3 copy one sample's depth; 4 to 6 average it, where Xenia copies one sample
>   (xenia:src/xenia/gpu/draw_util.cc:717-757). xsdk's depth resolves copy sample 0 at every count.
> - **Hierarchical depth at 2 and 4 samples,** at 1280x256 with xsdk's buffer and pass after a clear: hidden quads 84
>   times as fast, every visible pixel drawn, and milestone 30's and 49's draws after clears all right. xsdk keeps it
>   on up to 1280 wide and 1024 rows times samples.
> - **Rates,** by the GPU's clock: fills 3.4 Gpixels/s at 1, 2 and 4 samples, 6.9, 13.8 and 27 samples a clock,
>   blended as opaque; a 1280x720 resolve 270 us whole at 1 and 2 samples, and 277 in 3 bands at 1, 2 and 4.

Once the console list below has run, the section gains what xsdk_test and the breaks did.

## To run on the console

Build in WSL from the worktree: `make clean && make -j32`, then `sh tests/breaks/build.sh`. Then:
1. `build/xsdk_test.xex` with `net_host.txt` and `tools/net_echo.py` as usual: every check passes but the
   controller's when the pad is idle, the new ones above among them.
2. `build/breaks/xsdk_test_msaa_sample0.xex`, `xsdk_test_msaa_onesample.xex`, `xsdk_test_msaa_tiles64.xex` and
   `xsdk_test_msaa_bands2.xex`, one at a time, `msaa_bands2` from a fresh boot and last: each fails what "The breaks"
   names.
3. vulkan4xenon_test, twice, and vkbench, both built unchanged against this xsdk: as before.
4. Optionally `build/probe_msaa.xex`'s default group again: its `hiz_2` and `hiz_4` now run through xsdk's own
   hierarchical depth, and should log as before.
