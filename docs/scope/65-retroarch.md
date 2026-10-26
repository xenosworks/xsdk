# Milestone 65: RetroArch with XMB on vulkan4xenon

Agreed on 2026-10-25. The user asked for RetroArch on the console with its XMB menu, through vulkan4xenon and
today's xsdk, not the old XDK build, with the work spread over subagents.

## Decisions (the user, 2026-10-25)

1. **All seven cores** of step 4.
2. **PSP (PPSSPP) is the recompiler milestone after this one.**

## Sources

- **RetroArch** (libretro/RetroArch at tag `v1.22.2`, GPL-3.0), cited as `ra:<path>:<line>`; a local clone,
  `Z:\RetroArch-xenon`, branch `xenon` from the tag.
- **vulkan4xenon**, cited as `vulkan4xenon:<path>`; **SwanStation** as `swanstation-xenon:<path>`.

## What there is

- **RetroArch draws XMB through any of its drivers' display hooks;** its Vulkan driver has them, the ribbon among them
  (`ra:gfx/drivers/vulkan.c:5395-5413`, `pipeline_ribbon.vert` and `.frag`), with its shaders as SPIR-V beside their
  GLSL (`ra:gfx/drivers/vulkan_shaders/`). Its `khr_display` context makes the surface from `VK_KHR_display` alone
  (`ra:gfx/drivers_context/khr_display_ctx.c`), and griffin, the one-file build the console ports use, takes both
  (`ra:griffin/griffin.c:376-385, 746`).
- **vulkan4xenon has what that path asks for:** `VK_KHR_surface`, `VK_KHR_display` and `VK_KHR_swapchain`, uniform
  buffers, dynamic ones, combined image samplers and push constants, and the 16-bit formats a core's frame comes in,
  sampled with linear filtering (vulkan4xenon:docs/SUPPORT.md, Extensions, Descriptor sets, Formats). It compiles SPIR-V
  it hasn't bundled on the console the first time and keeps it (SUPPORT.md, Shaders). It has no compute: compute
  pipelines come back as `VK_NULL_HANDLE` (SUPPORT.md:43), and RetroArch makes some, to convert frames
  (`ra:gfx/drivers/vulkan.c:5479-5492`).
- **RetroArch loads Vulkan with `dylib_load`** (`ra:gfx/common/vulkan_common.c:5144-5180`); on xsdk it takes
  vulkan4xenon's `vkGetInstanceProcAddr`, linked in.
- **The console ports link one core into each executable** and switch cores by starting another one, the XDK's through
  `XLaunchNewImage` (`ra:frontend/drivers/platform_xdk.c:279-316`); xsdk has `xsdk_launch` and its launch data
  (`include/xsdk/xam.h:89-91`). `platform_xenon.c` is libxenon's 94-line stub.
- **xsdk has the rest:** sound (`audio.h`, milestone 58), pads through `XamInputGetState` and `XamInputSetState`
  (`kernel.h:728-729`), pthreads and files (milestones 54 and 55). SwanStation already runs a libretro core, with
  our PowerPC recompiler, on them (swanstation-xenon:src/xenon/host.c), and builds it as a static library
  (`swanstation_libretro`).
- **Most libretro cores build big-endian,** for RetroArch's PS3, Wii and Wii U ports: `-DMSB_FIRST` and a static
  build in their `Makefile.libretro`.

## The plan

1. **The frontend** (agent 1, RetroArch worktree `m65-frontend`): `Makefile.xsdk` on griffin, as `Makefile.wiiu` and
   `Makefile.libnx` are, with Vulkan, the menu with XMB and RGUI, rpng, the stb font, zlib and threads, and no
   networking; `frontend/drivers/platform_xsdk.c`, for paths under the `.xex`'s folder, the drives (`game:`, `hdd:`,
   `usb0:` to `usb2:`), the splash, and switching cores through `xsdk_launch` with the content's path as launch data;
   a salamander, `retroarch.xex`, which starts the last core used; the Vulkan loader linked to vulkan4xenon; XMB's
   monochrome icons and font from libretro/retroarch-assets in the package. Done when it builds with no core and with
   one.
2. **The video** (agent 2, vulkan4xenon worktree `m65-video`, RetroArch worktree `m65-video`): each of RetroArch's
   Vulkan shaders through xost (`tools/build_shaders.py`), into a bundle the build links, and what fails fixed in
   vulkan4xenon or worked around in RetroArch; every Vulkan call `vulkan.c`, `vulkan_common.c` and `khr_display_ctx.c`
   make held against SUPPORT.md, and the gaps closed the same way, the compute conversions skipped where the format
   samples directly.
3. **The drivers** (agent 3, RetroArch worktree `m65-drivers`): `audio/drivers/xsdk_audio.c` on xsdk's sound at
   48 kHz; `input/drivers/xsdk_input.c` and `input/drivers_joypad/xsdk_joypad.c` for four pads and rumble, with
   XMB's default binds.
4. **The cores** (agent 4, `Z:\cores-xenon\<core>`): each a static library with a `platform=xsdk` entry, and
   SwanStation's from swanstation-xenon. Proposed: FCEUmm (NES), Snes9x (SNES), Genesis Plus GX (Mega Drive, Master
   System, Game Gear, Mega-CD), Gambatte (Game Boy, Color), mGBA (Game Boy Advance), Beetle PCE Fast (PC Engine and
   its CD), SwanStation (PlayStation).
5. **On the console** (me): the pieces merged, one build and one run at a time.

Agents build and never run anything on the console; I review, merge and test.

## Checks

- **XMB on the console:** the ribbon drawn and moving, the icons and text, the menus walked with the pad, the content
  browser on each drive; the log says which shaders were compiled on the console and which came from the bundle.
- **Each core against the PC:** RetroArch's `--max-frames=N --max-frames-ss` runs a core on a test ROM for N frames
  and saves the last one (`ra:retroarch.c:7964-7993`). The console's frame is compared with one from the same core and
  ROM on the PC, pixel for pixel. Test ROMs: free homebrew only, such as the 240p Test Suite, which has NES, SNES,
  Mega Drive, Game Boy and PC Engine versions; the user's own games stay theirs.
- **Speed and sound:** each core at its system's frame rate in a timed run, with the audio's underruns logged.
- **Switching:** one core started from the salamander, another from its menu, the content passed through.
- **Breaks:** a core built without `-DMSB_FIRST` (its frame differs), the ribbon's shader left out of the bundle (made
  on the console instead, and logged), and launch data dropped (the next core starts with no content).

## Not in it

- **New recompilers.** N64 needs a PowerPC dynarec and an RDP drawn without compute: ParaLLEl-RDP is compute shaders,
  which Xenos doesn't have, and GLideN64 is OpenGL. Dreamcast and PSP have no PowerPC JIT either. Each is a milestone
  of its own.
- **Shader presets (slang),** netplay, the online updater, achievements and cores drawn through OpenGL or Vulkan.
- **The release:** a store package, and which cores' licences allow shipping them in one `.xex` with RetroArch's
  GPL-3: Snes9x's and Genesis Plus GX's are non-commercial.

## Size

Large. Step 2 is the risk: how many of RetroArch's shaders xost takes as they are isn't known until they're compiled.
