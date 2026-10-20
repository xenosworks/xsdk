# Milestone 44 step 5: the layer, Aurora and Dusklight at 2 and 4 samples

docs/scope/44-tiling-and-msaa.md, step 5; docs/HARDWARE.md, "Multisampling's cost, and Dusklight at 2 and 4 samples,
milestone 44 step 5". The layer's multisampling, its first part here, is recorded in
vulkan4xenon:results/2026-10-20-milestone-17-part-2/ with "A band's depth clear goes on a colour resolve" in
HARDWARE.md. Run on the console through the console agent on 2026-10-20; Ordon played by the user.

## What was built

- **vulkan4xenon** (`2e68725`, `e3d98e2`, `d435434`): `vkCmdResolveImage` of a multisampled depth image right after
  the pass that drew it resolves its sample 0 on the GPU, as xsdk's depth resolves do; `make V4X_FORCE_BANDS=n` forces
  every pass with depth into n bands, for measuring.
- **Aurora** (aurora-xenon `acbb442`): the vulkan4xenon backend draws the EFB at the sample count it's configured with
  where the layer takes it, colour averaged into the one-sample EFB its copies, the present and captures read, depth's
  sample 0 resolved where a depth copy or GXPeekZ reads it. At 1 sample it makes the same Vulkan calls as before.
- **Dusklight** (dusklight-xenon `b6010ceaeb`): CMake's `DUSK_XSDK_MSAA`, 1 unless set.
- **probe_tiles** times case 11's fill-bound and vertex-bound 720p frames at 2 samples in 2 and 3 bands and at 4 in 3.

## On the console

- `vulkan4xenon_test_1.log`, `_2.log`: 517 of 517; the depth resolves take sample 0, 0 texels wrong, 298 pixels at 2
  samples and 20 at 4 where samples 0 and 1 differ. `vulkan4xenon_test_depth_sample1.log`, xsdk's depth resolves
  copying sample 1: those two checks alone fail. `vkbench.log`: 17 of 17, within 3% of main's or faster.
- `probe_tiles_cost.log`, by the GPU's clock at 1280x720:

  | Frame | 1 sample untiled | 2 samples, 2 bands | 2 samples, 3 bands | 4 samples, 3 bands |
  |---|---|---|---|---|
  | Fill-bound | 10,302 us | 10,320 us | 10,325 us | 10,331 us |
  | Vertex-bound | 2,374 us | 4,487 us | 6,594 us | 6,594 us |

  A band's fixed cost 6 us (the bar 50); a predicated draw outside its band 0 us against 2,103 drawn (the bar a fifth).

## Dusklight

Diagnostics builds (`dusk_ms*.log`, local/m44c), the logo shot's phase 0 (nothing skipped):

| Build | GPU a frame from submit | Replay's CPU | Render worker |
|---|---|---|---|
| 1 sample | 13.2 ms | 1.1 ms | 3.3 ms |
| 1 sample, forced into 2 bands | 32.0 ms | 29.0 ms | 33.7 ms |
| 1 sample, forced into 3 bands | 31.6 ms | 28.7 ms | 33.0 ms |
| 2 samples | 17.0 ms | 11.2 ms | 13.2 ms |
| 4 samples | 18.7 ms | 12.8 ms | 14.7 ms |

- **Where the time goes:** a tiled pass replays its commands into a recording each band then runs, so banding costs
  the CPU most: forcing every pass with depth into bands at 1 sample took the replay from 1.1 ms to 29 ms a frame, the
  GPU's own work after the replay 3 ms. At 2 and 4 samples only the EFB's passes tile: 11 to 13 ms of replay, the GPU
  4 to 6 ms more than at 1 sample, its EFB passes each resolving colour twice.
- **The title scene,** every build's diagnostics at about 29.3 fps over its 10 s windows; `dusk_ms2_plain.log`, 2
  samples without the diagnostics, 28.8 to 29.9 fps, where 1 sample ran 29.7 to 29.9 (milestone 48 step 7).
- **Ordon** (`ordon_dusk_ms1.log`, `ordon_dusk_ms2.log`, played): at 1 sample the village (F_SP103) at 29 to 30 fps
  once settled, the GPU 12 to 16 ms a frame, F_SP104 at 29.0 to 29.4, 16 to 17 ms; at 2 samples the village at 27 to
  28 fps, 17 to 24 ms, F_SP104 26 to 30 fps, 22 to 27 ms.
- **The frames:** the logo shot at 2 and 4 samples differs from 1 sample at geometry edges, blended where 1 sample
  stair-steps (`edge_1_and_2_samples.png`, a 160x100 crop of the bridge's edge at 4 times, 1 sample left); forced
  into 2 bands it equals 1 sample; in 3 bands it differs only in band 0's sky, by about a level, the title scene's sky
  moving with time. The capture after phase 9 is black in every build, a fade.

**So Dusklight stays at 1 sample:** 2 samples holds neither the title scene nor Ordon at 29.5 fps, the scope's bar.
