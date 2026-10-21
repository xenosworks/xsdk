# Milestone 57 on the console (2026-10-21)

Each title's log, unchanged, from runs by the console agent. [SHA256SUMS](SHA256SUMS) has their hashes.
docs/HARDWARE.md, "Milestone 57 on the console", says what they found.

- `xsdk/`: [xsdk_test](xsdk/xsdk_test/xsdk_test.log) at `a87c3b7`, 628 passed; the 2 failures are the controller's
  state and rumble, with no one at the controller. Then the 9 breaks of [cases.tsv](../../tests/breaks/m57/cases.tsv),
  one folder each, each failing the checks its line names.
- `imgui/`: [imgui_check](imgui/imgui_check/imgui_check.log) from imgui-xenon `7e00ad8e`, 7 of 7, and its 9 breaks
  (`examples/example_xsdk/breaks` there), one folder each: each fails its own check, and `half_pixel` the font check
  too, its samples moved off the texels' centres.
- [imgui_check_linear.log](imgui/imgui_check_linear.log): imgui_check from imgui-xenon `4ba75a8d`, before its font
  check sampled point: the font check failed, eight pixels off by up to 8, the same in two runs.
