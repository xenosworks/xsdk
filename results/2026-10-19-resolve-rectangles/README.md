# Resolve rectangles kept, 2026-10-19

A resolve draws its rectangle from three vertices xsdk keeps in the write-back page, a slot for each place and size
(src/gpu.c, `rectangle_slot`). With 8 slots, a resolve at a ninth waited for the GPU to finish the oldest's, through
`xsdk_gpu_wait`'s 1 ms sleeps. vulkan4xenon milestone 29's profile found Dusklight's 27 passes a frame waiting there
about 10 ms of every frame (vulkan4xenon:results/2026-10-19-milestone-29). xsdk keeps 64 now, past the stream's
fences in the page.

`test_resolve_rect` resolves 96 8x8 rectangles, each its own place, more than xsdk keeps, and checks each landed and
that queuing them took under 10 ms. On the console, through the console agent, with the rect worktree pair (xsdk
from `bd84bf9`, vulkan4xenon from its milestone 29 step 3, `0a1fa72`, as its profile build):
- **`xsdk_test.log`:** 461 passed, 2 failed, the controller's checks with no pad. The 96 cells queued in 1,484 us, no
  texel wrong; 464 and 1,620 us in earlier runs of the same code.
- **`xsdk_test_8_slots.log`, broken on purpose:** 8 slots, as before. The cells queued in 21,264 us and that check
  failed, alone with the controller's.
- **`vulkan4xenon_test.log`:** 492 of 492.
- **`dusklight_profile.log`:** its title scene's submit took 1.06 ms a frame, 244 to 249 draws, from 10.5 to 11.9 ms,
  and its pass ends 131 to 140 us a frame, from 9.4 to 10.9 ms. The frame rate stays at the game's 30.
