# Milestone 49: the pass after a depth clear

Agreed on 2026-10-19.

## Why

A resolve that clears depth over a whole target draws its rectangle a second time, depth-only, testing hierarchical
depth (`put_hiz_test`, src/gpu_resolve.c:248-264). Without that pass, a draw's first block after a clear tests what the
buffer held before: 199 pixels in the top right 32x8 pixels of a full-screen quad. A full-screen draw testing the
buffer puts them right; one-pixel draws left 71 of the 199 (docs/HARDWARE.md, "Hierarchical depth, milestone 30").

The pass costs a resolve clearing depth about 29 us at 1280x720. It came out at 319 us against 290 for a resolve
clearing colour alone (same section). That is 921,600 pixels at the rate the buffer rejects them. Most frames clear
depth once, so the whole of this milestone is about 29 us a frame, 0.2% of a frame at 60 fps.

The pass is also drawn where the buffer then stays off: on multisampled targets and on targets past 1280x1024
(src/gpu_draw.c:259 against src/gpu_resolve.c:297).

## What it takes

1. **A probe of cheaper ways to put the buffer right after a clear.** Each case runs from a fresh boot, as a hang
   would leave the GPU hung. Each is checked with milestone 30's cases (probe_hiz_discovery's after a clear far, a
   draw at 0.9, and the small rectangles) and xsdk_test's 12 frames of near and far rectangles.
   - The pass drawn smaller: the top 8, 16 or 32 rows, the top right 32x8 block, or a quarter of the target. Passes in
     depth-only mode have never hung.
   - An event in place of the pass: `EVENT_WRITE` types 4, 10 and 20. CACHE_FLUSH, TILE_FLUSH, CACHE_FLUSH_AND_INV,
     CONTEXT_DONE and VIZQUERY changed nothing, and 9 and 21 hung (docs/HARDWARE.md, "Hierarchical depth, milestone
     30", and "Queries and events"), so this probe doesn't send them.
2. **xsdk takes the cheapest case that puts every pixel right.** It also skips the pass wherever the buffer stays off.

## Verification, without the user

- **Probe and xsdk_test:** the probe's cases and xsdk_test's hierarchical depth checks on the console, through the
  console agent.
- **Timing:** a resolve clearing colour and depth, against one clearing colour alone, timed on the GPU's clock.
- **vulkan4xenon_test**, twice.
- **A break build,** the pass left out, failing the checks after a clear.

## Done when

A resolve clearing depth costs at most 5 us more than one clearing colour alone, and every check after a clear holds.
If no case puts every pixel right, the probe's results are recorded, the pass stays as it is, and only the skip lands.
