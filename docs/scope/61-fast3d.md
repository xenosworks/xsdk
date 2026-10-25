# Milestone 61: Fast3D on the GPU layer

Agreed on 2026-10-22, with every recommendation taken (Decisions, below).

libultraship's games (Starship, Ship of Harkinian, 2Ship and the rest) draw through Fast3D, which has an OpenGL, a
Direct3D 11 and a Metal backend and none for xsdk. This milestone writes one on xsdk's GPU layer and brings Starship up
on it, so a libultraship game pays the GPU layer's cost for a draw rather than vulkan4xenon's, and measures what that
cost is in a real game.

## Sources

- **libultraship** (Kenix3/libultraship at `eaaf9d0`, MIT, the commit Starship pins), cited as `lus:<path>:<line>`.
- **Starship** (HarbourMasters/Starship at `3322643`, CC0), cited as `starship:<path>:<line>`.
- **Torch** (HarbourMasters/Torch at `cd92cc0`, Starship's submodule), cited as `torch:<path>:<line>`.
- **prism-processor** (KiritoDv/prism-processor at `bbcbc7e`), cited as `prism:<path>:<line>`.

## Why

- **The cost of a draw.** A cube costs the CPU 0.289 us a draw through xsdk's GPU layer and 0.966 through vulkan4xenon,
  0.670 of it the submit (vulkan4xenon:docs/ROADMAP.md, milestone 29). A Fast3D game draws many small batches, so the
  layer it draws through decides much of its frame.
- **Fast3D has no backend that runs on xsdk.** A backend implements `GfxRenderingAPI`, 40 methods
  (lus:src/graphic/Fast3D/backends/gfx_rendering_api.h:30-74), and a window backend implements `GfxWindowBackend`
  (gfx_window_manager_api.h:6-38). Fast3D picks among DX11, SDL with OpenGL and SDL with Metal
  (lus:src/graphic/Fast3D/Fast3dWindow.cpp:123-137; lus:src/window/Window.h:13).
- **What Fast3D asks of a backend is small.**
  - It transforms and lights every vertex on the CPU (`GfxSpVertex`, lus:src/graphic/Fast3D/interpreter.cpp:1155-1336)
    and culls on the CPU (`GfxSpTri1`, 1362-1404). The vertex shader only passes its position on
    (lus:src/graphic/Fast3D/shaders/opengl/default.shader.vs:75).
  - `DrawTriangles` takes up to 256 triangles of floats (interpreter.cpp:105, 110, 125-131): 4 for the clip-space
    position, then texture coordinates, clamps, fog, grayscale and the combiner's inputs as the shader uses them
    (1617-1762).
  - A batch ends at a change of depth test, mask or decal, viewport, scissor, texture or sampler, shader or alpha blend,
    a full buffer, or a framebuffer command (1409-1608, 1771-1773, 3458-3564).
  - Textures arrive as RGBA8 bytes (lus:src/graphic/Fast3D/backends/gfx_opengl.cpp:538-539).
- **Starship needs none of the readbacks.** Nothing in it calls `GetPixelDepth`, `ReadFramebufferToCPU` or
  `CopyFramebuffer`, or uses the framebuffer commands that reach them; its sun and lens flare are placed from the
  camera's angles (starship:src/engine/fox_bg.c:1102-1103, 1112, 1176-1177), and its blur is unused
  (starship:src/engine/fox_blur.c:6). Ship of Harkinian's depth reads come with it, later (Left out).

## What Starship needs besides the backend

- **SDL2.** libultraship and Starship call 74 SDL2 functions: windows and GL contexts, events, game controllers with
  rumble, LEDs and sensors, a queued audio device, timers, hints, message boxes and the pref path
  (lus:src/graphic/Fast3D/backends/gfx_sdl2.cpp; lus:src/controller/; lus:src/audio/SDLAudioPlayer.cpp:12-40). The
  window and GL calls belong to the window backend, which this milestone replaces. SDL-xenon is SDL3. sdl2-compat
  (libsdl-org/sdl2-compat, Zlib) puts SDL2's API on SDL3, but its CMake refuses a static build outside Linux and QNX
  (sdl2-compat:CMakeLists.txt:88, 96-97). Step 1 finds out what xsdk needs from it.
- **Libraries.** libultraship's: libzip, nlohmann_json, tinyxml2 and spdlog by `find_package`
  (lus:src/CMakeLists.txt:191-201); ImGui `v1.91.9b-docking`, thread-pool `v4.1.0`, prism and stb_image fetched
  (lus:cmake/dependencies/common.cmake:9-15, 55, 97-113). Starship's: dr_libs, Ogg and Vorbis
  (starship:CMakeLists.txt:227-232, 326-367). StormLib and libgfxd are off by default (common.cmake:39-50, 69-75).
- **Torch stays on the PC.** Starship links Torch into the game to extract a ROM it's given
  (starship:CMakeLists.txt:386-387; starship:src/port/extractor/GameExtractor.cpp:61-64), which needs yaml-cpp and the
  rest of Torch's dependencies (torch:CMakeLists.txt:213-245). Its README already has Switch users make `sf64.o2r` on a
  PC and copy it over (starship:README.md:48-50). The console does the same, and the game's own extraction is built
  out.
- **Assets.** `sf64.o2r` from a US 1.0 or 1.1 ROM (starship:config.yml) and `starship.o2r`, the port's own, beside the
  `.xex` (starship:src/port/Engine.cpp:75-76; lus:src/Context.cpp:458-526).
- **Big-endian.** libultraship reads a resource in the byte order its header names and swaps when that isn't the
  host's (lus:src/resource/ResourceLoader.cpp:249-250; lus:src/utils/binarytools/BinaryReader.cpp:57-169), and Torch
  writes the PC's (torch:src/factories/BaseFactory.cpp:4), so a little-endian archive is meant to load here. It has
  never been tried on a big-endian host. Places that assume a little-endian one:
  - unconditional `BSWAP16` of data in the ROM's order (starship:src/port/resource/importers/audio/SampleFactory.cpp:36;
    starship:src/audio/audio_load.c:402; audio_seqplayer.c:912, 1017);
  - `BE16SWAP_CONST` in static envelopes, a no-op on a big-endian host (starship:src/audio/note_data.c:203-206);
  - `LightEntry` read raw (lus:src/resource/factory/LightFactory.cpp:14);
  - `IS_BIG_ENDIAN` in `gbi.h`, a spelling defined nowhere (lus:include/libultraship/libultra/gbi.h:1691), where
    libultraship defines `IS_BIGENDIAN` (lus:src/utils/binarytools/endianness.h:51-55).
- **Threads.** An audio thread in lock-step with each frame (starship:src/port/Engine.cpp:362-436), a resource pool of
  hardware threads less 4 (lus:src/resource/ResourceManager.cpp:55-56; Engine.cpp:165, 171), spdlog's one async thread
  (lus:src/Context.cpp:104, 153) and a decoder thread a streamed sample (SampleFactory.cpp:214-219). Each is placed on
  a hardware thread of its own where the measurements say so (docs/PARITY.md, "Placing threads").
- **The frame.** The game updates at 60 / `gVIsPerFrame`, 30 a second by default (starship:src/sys/sys_main.c:87),
  and draws at `gInterpolationFPS`, 60 by default, running the display list once per interpolated pass
  (starship:src/port/Engine.cpp:489-547).

## The design

### The backend, `gfx_xsdk.cpp`

In the libultraship fork, beside the other backends, selected by a `WindowBackend` value of its own.

- **Shaders.** Fast3D asks for a shader the first time a combiner is used, keyed by two IDs
  (lus:src/graphic/Fast3D/backends/gfx_opengl.cpp:375-415); prism expands a template's `@if` and `@for` for it
  (prism:src/prism/utils/invoke.c:8-60). The OpenGL template is GLSL 1.10 and 1.30 (gfx_opengl.cpp:286, 345). The
  backend has a template of its own, the OpenGL one's logic in the GLSL glslang takes for Vulkan, compiled on the
  console by glslang and `xsdk_xsc_compile` (`xsc.h`) and kept on disk, as Aurora keeps Dusklight's
  (aurora-xenon:lib/vulkan4xenon/program.cpp). What the template uses xost already takes (xenos-toolchain/docs/xsc.md):
  `if` on a uniform, `discard`, `fract`, `mix`, `pow`, `gl_FragCoord.xy`, and no bitwise operations. Its one call
  xost lacks, `textureSize`, sizes the mask textures (default.shader.fs:110-112); the backend passes their sizes as
  uniforms, as Fast3D already passes the textures' (`texture_width`, `texture_height`; gfx_opengl.cpp:62-73).
- **Vertices.** Each batch's floats go into a ring of write-combined memory a frame, 32-bit stores or VMX stores to a
  16-byte-aligned address (CLAUDE.md), and draw from there through one vertex fetch a shader. No descriptor sets, no
  recording, no replay: each call writes its packets as the game calls it.
- **Constants.** The few uniforms the template reads, set through xsdk's float constants when they change
  (`frame_count`, `noise_scale` and the texture sizes and filtering; gfx_opengl.cpp:57-73).
- **Textures.** RGBA8, tiled, `XSDK_FORMAT_R8G8B8A8`, many to an allocation: `xsdk_texture_init` and
  `xsdk_texture_attach` place a texture at any 4 KB in memory from `xsdk_gpu_alloc` (include/xsdk/texture.h:170-175),
  where `xsdk_texture_create` takes 64 KB pages of its own (docs/PARITY.md, "GPU memory"). A texture written while the
  GPU may read it (texture.h:179) takes new memory, and the old goes once the frame that read it is done.
- **State.** Depth test and mask, `LESS` or `LEQUAL` for decals with a depth bias (gfx_opengl.cpp:598-645), alpha
  blending (589-595, 671), scissor and viewport, and samplers with Fast3D's four wrap modes (548-570) as
  `XSDK_ADDRESS_*`.
- **Depth clamp.** Fast3D turns on `GL_DEPTH_CLAMP` (gfx_opengl.cpp:668). Xenos turns clipping off with
  `PA_CL_CLIP_CNTL`'s `clip_disable` (xenia:src/xenia/gpu/registers.h:547), which Xenia takes as depth clamp
  (xenia:src/xenia/gpu/d3d12/pipeline_cache.cc:1508). xsdk sets that register but has no setting for the bit
  (src/gpu_draw.c:804-813). xsdk's raster state gets one, with an xsdk_test check on the console of what the GPU does
  with depth past the near and far planes.
- **Framebuffers.** Fast3D draws the game into a framebuffer of its own when its size differs from the window's or
  multisampling is on (interpreter.cpp:4270-4287), and shows it through `GetFramebufferTextureId` (4343-4348): an eDRAM
  target resolved into a texture, drawn into the front buffer. 1280x720 at 1 sample to start (Decisions, 6).
- **The rest.** `GetPixelDepth`, `ReadFramebufferToCPU` and `CopyFramebuffer` log their first call and return
  nothing, until a game that calls them is brought up (Left out).

### The window backend

`GfxWindowBackend` on `xsdk_display_*` and `xsdk_present_*`: one 1280x720 window, always full screen, its refresh 60,
`IsFrameReady` pacing the frame to the game's target, and no mouse.

### ImGui

libultraship builds ImGui's OpenGL 3 and SDL2 backends (common.cmake:30-34) and picks its renderer by window backend
(lus:src/window/gui/Gui.cpp:174, 222, 346, 805). The xsdk window backend draws ImGui through imgui-xenon's backend for
the GPU layer, built against libultraship's ImGui.

## What it takes

1. **Bring-up without drawing.** The forks, the libraries built for `powerpc-xsdk-elf`, SDL2 through sdl2-compat,
   Torch built out, and a backend that counts its calls and draws nothing. Starship boots from its archives, plays its
   sound, and logs a frame's draws, triangles, shaders, texture uploads and the CPU time of the game, the interpreter
   and the backend, each 10 s. The big-endian list above is fixed as it's reached.
2. **The backend draws.** Shaders, vertices, constants, textures, state, depth clamp, the game's framebuffer, the
   present and ImGui. The title screen and Corneria drawn on the console.
3. **Measured and tightened.** Each step measured as milestone 34 measured Aurora: the backend's CPU time a draw and a
   frame, the GPU's time a frame, and the frame rate, at fixed places; then the backend's costs taken out where the
   numbers point. A shader pack built from what a session compiled.
4. **Checks and breaks, then the runs** below.

## Checks

- **xsdk_test:** depth clamp, a triangle crossing the near plane drawn with clipping off and on, and its depth read
  back.
- **The backend's own checks**, a title in the libultraship fork drawing Fast3D's paths from a recorded display list:
  a combiner of each kind, both texture slots, each wrap mode, decals, alpha, fog and the game's framebuffer, each read
  back from the GPU and compared with its expected texels.
- **Broken on purpose,** each failing what it names: textures sharing an allocation written at the wrong offset; a
  stale constant (the last batch's texture size); depth clamp off.

## Verification

- By the agent: xsdk_test and the backend's checks, twice each, and the breaks; Starship's title screen and Corneria
  captured at a counted frame, and the logged costs.
- By the user, once at the end: Starship played from the title through Corneria, about 10 minutes.

## Done when

- xsdk_test and the backend's checks pass twice, and each break fails what it names.
- Starship plays through Corneria on the console through `gfx_xsdk`, its captures matching what the user sees
  (Decisions, 14: no longer the PC's OpenGL build too).
- The backend's CPU time a draw, not counting the copy of its vertices, its parts, the copy's rate and the share of
  the frame each part takes are recorded (Decisions, 13; it was to be at most 0.6 us, twice the GPU layer's own cost
  for a draw).
- Corneria holds the game's own 30 fps: every settled 10 s window at 29.5 fps or more, milestone 30's bar. The
  interpolated 60 is measured and recorded, not required (Decisions, 5).

## Left out

- `GetPixelDepth`, `ReadFramebufferToCPU` and `CopyFramebuffer`, which Ship of Harkinian's depth reads and
  framebuffer effects need: with that port.
- Internal resolutions past 1280x720, through tiled passes (milestone 44), and multisampling: after Corneria holds 30.
- Extracting a ROM on the console, the Japanese, European and Chinese ROMs, keyboards and mice, and mods built for
  other backends.
- Other libultraship games. The backend is written for all of them, and brought up on one.

## Decisions

All taken by the user on 2026-10-22, the last four as recommended:
1. **The title:** Starship, the smallest libultraship game and in C, in xenosworks forks of Starship and
   libultraship.
2. **SDL2:** sdl2-compat on SDL-xenon, forked where its static build needs it.
3. **Numbering:** xsdk milestone 61.
4. **Shaders:** compiled on the console at first use and kept on disk, a pack built later from what sessions
   compiled, as Dusklight does, rather than a pack alone from a list recorded on the PC. A combiner's first use
   hitches, as Dusklight's pop-in does, and nothing has to be listed in advance.
5. **The frame rate the milestone holds:** the game's own 30, the interpolated 60 recorded, not required. The game
   runs the display list once per interpolated pass, so 60 doubles the CPU's work, and how far the backend gets toward
   it is what step 3 measures.
6. **Resolution:** 1280x720 at 1 sample, 1920x1080 through tiled passes once 720p's costs are known.
7. **The repositories:** xenosworks/libultraship-xenon from `eaaf9d0` and xenosworks/starship-xenon from `3322643`,
   GitHub forks with default branch `xenon`, made by me at the user's word on 2026-10-22; xenosworks/sdl2-compat-xenon
   only if step 1 patches sdl2-compat.

Taken by the user later on 2026-10-22, after steps 1 and 2 ran on the console:

8. **Step 3 takes the interpreter's vertex work to the console's hardware without a profiling pass first:** first VMX
   in Fast3D's per-vertex transform, lighting, texgen and fog, then the vertex shader doing that math on the GPU. Step
   1 had found Fast3D's interpreter, not the cost of a draw, holding the attract demo to 41 fps at the interpolated 60
   (`local/m61/runs/step1`, about 100 draws and 13 to 14 ms of interpreter a frame), so this widens step 3 from the
   backend to the interpreter. The 10 s log lines give each change's before and after.
9. **The GPU vertex path is reverted, VMX kept** (the user, on the A/B's numbers). VMX matched the scalar code on every
   one of about 17 million vertices on the console and cut the interpreter's heavy scenes from 9 to 11 ms a frame to 5
   to 7. The vertex shader doing GfxSpVertex's work drew the same picture in two of three captured frames, but cost
   more: the CPU built 580 to 760 vertex states a frame, as N64 display lists change matrices per object part, which
   took longer than VMX's transform, about twice the triangles reached the GPU without the CPU's culling, and the
   interpreter's time went up, 6.0 to 7.6 ms against 3.6 to 6.6; one frame drew triangles beyond the far plane that the
   CPU rejects. It stays on libultraship-xenon's branch `gpu-vertex-experiment`.

Taken on 2026-10-25, after the user heard the sound crackle:

10. **Starship draws at the game's own 30 by default on the console.** At the interpolated 60 the heavy scenes made
    25.6 to 28.9 game updates a second, and the game makes its audio a frame an update, so the audio queue ran dry up
    to 92 times in 10 s. At 30 the updates held 30.0 a second outside scene loads, and the queue never ran dry there
    (starship-xenon `4a87bc39`; docs/HARDWARE.md, "Starship's audio follows its updates, milestone 61"). A rate the
    player picks in the menu still wins. This is decision 5's frame rate as the default.
11. **The audio gaps at a scene load stay until loading is faster** (the user). Loading a scene's resources holds one
    update 130 to 440 ms, past the queue's 77 ms, as the audio runs in lockstep with the game (TODO.md, "Starship's
    scene loads stall its audio"). Letting the audio thread run on its own clock was the other way, not taken.
12. **The vertex ring is invalidated once a segment, its draws a page each** (the user, on the numbers): the
    per-draw invalidate cost 0.26 us of a draw, and drew nothing different (docs/HARDWARE.md, "A Fast3D draw's
    parts, milestone 61").
13. **A draw's cost is recorded, not required to be 0.6 us** (the user). A heavy scene's draw costs 1.18 to 1.35 us
    less its copy, most of it the dwords a texture bind and a block of constants write into the ring, which an N64
    display list needs nearly every draw; the backend takes under 1% of Starship's frame (docs/HARDWARE.md, "A Fast3D
    draw's parts, milestone 61").
14. **No comparison with the PC's OpenGL build** (the user). Building Starship for the PC in WSL needs packages only
    the user can install; the console's captures are checked against what the user sees.
15. **The hiccups of the user's plays are taken out where they start** (the user asked for them repaired). Starship
    records interpolation only for frames it replays (starship-xenon `ae491df6`), the log reserves its file's room
    (src/log.c), and libultraship reads its archive into memory (libultraship-xenon `fe2122f7`); saves stay as
    libultraship writes them (docs/HARDWARE.md, the four sections from "The map at 30: recording only what's
    replayed, milestone 61").

## Size

Large. Step 1 is most of it: about eight libraries and sdl2-compat to build for xsdk, Starship's start-up, and the
big-endian sites, the size of the list above not yet known. The backend is about the size of the OpenGL one, 40 KB of
code (lus:src/graphic/Fast3D/backends/gfx_opengl.cpp), with its template, the window backend and the ImGui glue.
xsdk's part is the depth clamp setting and its check.

## Risks

- **The CPU.** Fast3D transforms, lights and clips every vertex on the CPU, and libultraship is C++ with maps and
  logging in its paths. On Xenon's in-order cores the game's CPU may cost more than the draws, and then the backend's
  savings don't show in the frame rate. Step 1 measures it before the backend exists.
- **Big-endian.** Sites beyond the list above, found only as the game reaches them, in audio above all.
- **sdl2-compat** may load SDL3 at run time even when built static, which xsdk can't; then a shim of the functions
  Starship uses takes its place, and the decision goes back to the user.
- **ImGui's version.** libultraship pins `v1.91.9b-docking` with patches (common.cmake:9-15); imgui-xenon's backend
  follows its own ImGui, and may need changes to build against libultraship's.
- **Depth clamp** may not be what `clip_disable` gives on the console, as only Xenia says so; the xsdk_test check
  settles it before the backend counts on it.
