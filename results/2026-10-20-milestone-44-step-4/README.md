# Milestone 44 step 4: alpha to coverage

docs/scope/44-tiling-and-msaa.md, step 4. The probe ran on the console through the console agent on 2026-10-20, built
from `069ad03` (`make msaa_probes`): its six alpha to coverage cases, each alone from a fresh boot, as they set
`RB_COLORCONTROL`'s `alpha_to_mask_enable`, which xsdk never had. Logs: `probe_msaa_<case>.log`, copied from
`local/m44p2/atoc`. The build on top of it is not yet run on the console; the list to run is at the end.

## What the probe found

Each case cleared a 256x128 target with a resolve, drew 17 squares of 16x16 at alphas 0 to 1 in 16ths, on even pixels
so 2x2 quads line up, with `RB_COLORCONTROL` set for the squares and xsdk's 0x87000007 put back before each resolve,
then resolved every sample alone (copy sample selects 0 to n - 1) and once through the averaging select. Each first
drew its control, alpha to mask off.

- **No case hung,** and each ended with `xsdk_gpu_close` 0.
- **The controls** covered every sample at every alpha, 68 of 68 quad pixels, at 1, 2 and 4 samples. In every pass, of
  the controls and of the cases, every pixel was the shape's colour or the clear under each select, every pixel of a
  square was like its first quad, nothing off the squares was covered, and at 2 and 4 samples the averaging select
  gave the selects' count.
- **Offsets 2, 2, 2, 2** (`RB_COLORCONTROL` 0xAA000017, `probe_msaa_atoc_sym_<n>.log`) made every pixel of a quad cover
  alike, and coverage only grew with alpha. The first alpha covering n samples:

  | Samples | 1 | 2 | 3 | 4 |
  |---|---|---|---|---|
  | 1 | 8/16 | | | |
  | 2 | 4/16 | 12/16 | | |
  | 4 | 2/16 | 6/16 | 10/16 | 14/16 |

  That is the share of a pixel's samples nearest its alpha, a tie rounding up: alpha n/4 covers n/4 of each pixel's
  samples wherever that is whole (20 of 20, 12 of 12 and 8 of 8 pixels). Over a quad, n/4 at alpha n/4 held at 4
  samples only (2 of 5 alphas off at 2 samples, 3 of 5 at 1), as at 1 and 2 samples every pixel steps at once.
- **libxenon's 3, 1, 0, 2** (0x87000017, `probe_msaa_atoc_dither_<n>.log`) gave each pixel of a quad its own
  thresholds, and the quad's total was n/4 of its samples at alpha n/4 at every count. At 4 samples pixel (0, 0) went
  from 1/16, 5/16, 9/16 and 13/16, (1, 1) from 2/16, (1, 0) from 3/16 and (0, 1) from 4/16, each 4/16 apart.
- **The rule that fits all 24 pixels:** with offset o, a pixel's n-th of N samples is covered from alpha
  (4n - o) / (4N). That is Xenia's reading, the offsets lowering the thresholds by o / 4 / N
  (xenia:src/xenia/gpu/registers.h:710-721).
- **Which pixel takes which offset:** offset i went to the pixel at x = i & 1, y = i >> 1, offset 1 to (1, 0) and
  offset 2 to (0, 1), as Xenia's code picks it (xenia:src/xenia/gpu/dxbc_shader_translator_om.cc:1881-1894); Xenia's
  comment (registers.h:722-726) and the probe's labels name them the other way round. With 2, 2, 2, 2 it doesn't
  matter.
- **Which samples go first:** sample 0, then 1, 2 and 3 in index order (masks 1, 3, 7, F), at every pixel and offset.
- **At 1 sample** alpha to coverage is an alpha test at 1/2 with offsets 2, 2, 2, 2.

## What's built

- **`XsdkBlend.alpha_to_coverage`,** and `XsdkBlendState.color_control`, `RB_COLORCONTROL`'s value:
  `alpha_to_mask_enable` (bit 4) and offsets 2, 2, 2, 2, 0xAA000017, when on; xsdk's 0x87000007 when off; every other
  bit as before (xenia:src/xenia/gpu/registers.h:682-731). `XsdkBlendState` grows from 8 bytes to 12.
- **`xsdk_gpu_set_blend` and `xsdk_gpu_set_blends`** write it with `RB_BLENDCONTROL0`, one packet of two registers.
  `xsdk_gpu_set_blends` takes `states[0]`'s, as `RB_COLORCONTROL` is one register for every target, and Xenia takes
  oC0's alpha (xenia:src/xenia/gpu/dxbc_shader_translator_om.cc:1858-1863); gpu.h says so.
- **What else writes `RB_COLORCONTROL`:**
  - the target set-up, `put_target` in src/gpu_draw.c, xsdk's 0x87000007 as before, for every `xsdk_gpu_set_target*`
    and `xsdk_gpu_set_targets*` and each band of a tiled pass;
  - now each resolve, 0x87000007 too, so a resolve copies with alpha to mask off, as every resolve of the probe ran.
    `RESOLVE_MAX_DWORDS` goes from 160 to 168, as milestone 49's probe build would otherwise fill it exactly.

  So setting a target or resolving undoes it, as gpu.h says beside `xsdk_gpu_set_blend` ("Undone by
  xsdk_gpu_set_target and resolves"), and docs/GUIDE.md's new row for the two calls. A blend set before a tiled pass is
  undone by its bands' targets, as blending already was.
- **Initializers:** the tests' and probes' positional `XsdkBlend` initializers take `false` for the new field, as
  `-Wmissing-field-initializers` asks.
- **vulkan4xenon** builds against it only once `bench/backend_xsdk.c`'s two positional `XsdkBlend` initializers
  (lines 298-301) take `false` too, for the same warning. Built with that warning let through, the layer, its tests
  and vkbench built and linked; the layer's own initializers are designated, and its `memcmp` of `XsdkBlendState`
  arrays (src/queue.c) holds, as `xsdk_gpu_blend_state` sets every field. Its objects must be rebuilt against this
  gpu.h, as `XsdkBlendState` grew.

## xsdk_test's new checks

In `tests/test_multisample.c`, after step 3's. For each count a 1280x64 target draws 17 squares of 48x48 at alphas 0 to
1 in 16ths, on even pixels, then one more at 1/16 after the target is set again, all under
`XsdkBlend.alpha_to_coverage`, each count in its own colour. The bands are resolved into rows 224, 320 and 416 of a
1280x720 frame, shown on screen together for `TEST_SHOW_MS`. Every pixel of each band is read back: on a square, red,
green and blue the clear plus k/n of the colour over it within a step of 8 bits, k the samples the table above covers
at its alpha; off the squares, the clear. Alpha is left out, as the squares' differ, as the probe read them. Each band
logs the samples covered at each square, to read a break's run by.
- `multisample: alpha to coverage at 1 sample: each square covers the samples the probe found at its alpha, every
  pixel, the rest the clear`
- `multisample: alpha to coverage at 2 samples: ...`, the same.
- `multisample: alpha to coverage at 4 samples, through xsdk_gpu_set_blends: ...`, the same; 1 and 2 samples go
  through `xsdk_gpu_set_blend`.
- `multisample: alpha to coverage: off again once the target is set, alpha 1/16 covering every sample at 1, 2 and 4
  samples`
- `multisample: alpha to coverage: the squares shown at 1280x720`, which fails in Xenia, which has no display
  controller.

## The breaks

`sh tests/breaks/build.sh` in WSL builds `build/breaks/xsdk_test_<break>.xex` from `tests/breaks/<break>.patch`, with
milestone 51's and step 3's. Each must fail these checks, and the controller's when the pad is idle, and pass the rest:
- **`xsdk_test_atoc_noenable.xex`,** alpha to mask's enable bit left off (0xAA000007): every square covers every
  sample, so `multisample: alpha to coverage at 1 sample: ...`, `... at 2 samples: ...` and `... at 4 samples, through
  xsdk_gpu_set_blends: ...` fail; each band logs n for every alpha.
- **`xsdk_test_atoc_offsets0.xex`,** the offsets left at 0 (0x00000017): each threshold moves up half a step, to 16/16
  at 1 sample, 8/16 and 16/16 at 2, and 4/16, 8/16, 12/16 and 16/16 at 4, as the dithered cases' pixel with offset 0
  covered, so the same three fail.

`... off again once the target is set ...` and `... the squares shown ...` pass under both.

## On the console

docs/HARDWARE.md, "Alpha to coverage, milestone 44 step 4", records the probe and the run: `xsdk_test.log` 501 of
503 (the controller's two with the pad idle); `xsdk_test_atoc_noenable.log` and `xsdk_test_atoc_offsets0.log` each
failing the three coverage checks; `vulkan4xenon_test_1.log`, `_2.log` 492 of 492; `vkbench.log` 17 of 17, built with
vkbench's `XsdkBlend` initializers named (vulkan4xenon). Which pixel each dithered offset goes to is the other way
round from what this README said before the run: column i & 1, row i >> 1, where Xenia's code takes the column as the
high bit.
