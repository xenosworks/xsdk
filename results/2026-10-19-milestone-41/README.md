# Milestone 41: docs and samples

docs/scope/41-docs-and-samples.md. Built on branch M41, rebased on main b231db0, with the toolchain milestone 52 left
in `~/xsdk-gcc`; `make clean`, `make -j32`, `make samples` and `make sample_breaks` (`SDL3_DIR`, SDL-xenon's build)
gave 0 warnings. libxsdk.a is unchanged, so xsdk_test wasn't run again. Run on the console through the console agent
on 2026-10-19.

## The samples

Each sample's log, then each broken on purpose (`*_brk.log`):

| Sample | Its checks | Broken on purpose |
|---|---|---|
| hello | passes | clears to another colour: its one check fails |
| triangle | both pass | its vertices left in cached memory without the invalidate: the moved colours' check fails |
| cube | both pass | depth testing off: the front face's check fails, the corner's passes |
| sound | passes | its frames never submitted: its check fails |
| sdl3 | the frame's and the tone's pass; the gamepad's fails | its gamepad never opened: the gamepad's check fails |
| template | passes | |

sdl3's gamepad check fails both ways, as no controller was connected (as xsdk_test's controller checks fail without
one), so its break isn't shown yet. That, and what each sample shows and plays, waits for the user at the console.

## The fresh agents

Two subagents, each with nothing but the repository, its docs and a WSL install, went from no toolchain to a title of
their own, with toolchains in prefixes of their own:
- **twocubes** (`twocubes.log`): two cubes, each turned by a stick. It ran on the console and its three checks passed.
  It was held up or misled in five places, fixed in `34c1f1d` and `9eae98f`: where PREFIX and WORK go, a toolchain
  that make didn't hand on to CMake, a build folder that kept its first compiler, where buffers sit in one allocation,
  and which name a shader header takes.
- **checker** (`checker.log`): a scrolling, generated checkerboard that beeps on A, about 12 minutes from start to a
  built `.xex`. It ran on the console and its three checks passed, the middle pixel exactly the texel it uploaded. It
  asked eight smaller questions, answered in `d35eea2`: what a callback that submits no audio frame plays, floats in
  the callback, the audio memory's caching, libxsdk.a built before a title, skipped vertex input locations, which way
  v runs, whether texels arrive exactly, and the toolchain's last line.
