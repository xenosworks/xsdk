# xost's literal pool, 2026-10-19

`xsdk_xsc_literal_reg` pooled a literal by its components rounded to 9 places, so values closer than that shared one
register, whichever came first: 1e-10 read as 0 (or 0 as 1e-10), and small constants a hair apart read as one. Milestone
48's counting found it (`results/2026-10-19-milestone-48/README.md`, "Found on the way"). The pool now keys a literal by
the 32-bit floats its register holds, bit for bit (src/xsc_lower.c), and `xsdk_xsc_round9` is gone.

- **What changed:**
  - **Dusklight:** 21 of its 150 programs, each a vertex shader normalising a normal with `max(len2, 1e-10)` and
    `step(1e-10, len2)`, which had compiled both as comparisons with 0.
  - **vulkan4xenon's test bundle:** six programs, `aurora_texgen_zero.vert` and its instanced build, which use 1e-10,
    and the four `aurora_ztex_*` pixel shaders.
  - **Unchanged:** its blit and vkbench bundles, and xsdk's generated test shaders.
- **xost's tests:** `tests/test_literal_pool.py` runs `vk_literal_pool.frag` on the simulator, the optimiser off and
  on, and wants every export exactly. Against the old pool it failed: `max(v, 0.0)` gave 1e-10. Every other xost test
  passes.
- **On the console,** built clean from the M48 pair:
  - `xsdk_test.log`: 459 passed, 2 failed, the controller's checks with no pad.
  - `vulkan4xenon_test_1.log`, `_2.log`: 492 of 492 each, with the bundle rebuilt.
  - `probe_shader_cost.log`: every pixel shader's picture hashes as in milestone 48's baseline runs, as only vertex
    shaders changed.
  - `dusklight.log`: Dusklight with its pack rebuilt, the title scene at 26 to 30 fps, as before.
