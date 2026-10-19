# Tiled uploads a tile at a time, 2026-10-19

docs/HARDWARE.md, "Uploads in runs, and channel swizzles, milestone 29", its last paragraph; vulkan4xenon:docs/ROADMAP.md,
milestone 29 step 6. Built clean in the V29 worktree pair (xsdk from `1d14f53`), run on the console through the console
agent.

- **`xsdk_test.log`:** 463 passed, 2 failed, the controller's checks with no pad. A 512x512 R8 tiled upload took 461
  us, a 512x512 A8R8G8B8 one 948 us, and every test texture uploaded both ways matched byte for byte.
- **`xsdk_test_broken.log`:** the step between tiles not carrying across the bank and pipe bits; the byte-for-byte check
  failed, alone with the controller's.
- **`vulkan4xenon_test.log`:** 492 of 492.
- **`dusklight_profile.log`:** Dusklight against vulkan4xenon's profile build. In the attract movie, its two copies a
  frame took 460 to 544 us, from about 2.35 ms (vulkan4xenon:results/2026-10-19-milestone-29), and its submit 0.61 to
  0.69 ms a frame; the replay waited for the GPU for none of it.
