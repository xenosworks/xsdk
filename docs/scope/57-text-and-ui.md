# Milestone 57: text and UI

Agreed on 2026-10-21, with every recommendation taken (Decided, below).

The entry agreed with the user: a glyph atlas on the GPU layer, then Dear ImGui, for debug overlays and homebrew menus.

## Why

- **Nothing in xsdk draws text.** The splash writes its logo's pixels into the front buffer from the CPU
  (src/splash.c, `draw_logo`), and xsdk_test reports only to its log. A title that wants a number on screen brings a
  font and a renderer of its own.
- **Debug overlays.** The log reaches the PC (milestone 53), but a porter at the console sees nothing on the screen:
  no frame time, no counters, no state of the game.
- **Homebrew menus.** A homebrew shop is the first use the networking scope names (docs/scope/networking.md, "What
  first uses it"), and milestone 56 builds its networking with no screen. Menus need text and widgets driven by a
  controller.
- **Dear ImGui is what PC homebrew and ports already use** for both. Aurora builds it for xsdk today, 1.91.9b with
  `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS` (aurora-xenon:extern/CMakeLists.txt:229-251), and starts ImGui's frame on the
  console (aurora-xenon:lib/vulkan4xenon/backend.cpp:558-566), but its vulkan4xenon backend drops what ImGui draws
  (aurora-xenon:lib/vulkan4xenon/encoding.cpp:1281-1282). D3D9 titles draw it through imgui_impl_dx9, 585 lines that set
  render states, a texture and a scissor and draw indexed triangles (imgui:backends/imgui_impl_dx9.cpp, 1.91.9b): the
  GPU layer has each of those.
- docs/PARITY.md has no row for text, and lists chatpads and USB keyboards as No.

## What the GPU layer already gives text

Each piece below is proven on the console by an earlier milestone:
- **8-bit textures,** `XSDK_FORMAT_R8`, read as red, point or linear sampled (include/xsdk/texture.h; docs/HARDWARE.md,
  "Uploads in runs, and channel swizzles, milestone 29").
- **Blending, the scissor, culling off and depth off,** each a state block (include/xsdk/gpu.h; milestone 14).
- **Indexed triangle lists** of up to 65,535 big-endian 16-bit indices, a list starting on 4 bytes
  (docs/GUIDE.md, "Memory the GPU reads"; xenia:src/xenia/gpu/command_processor.cc:1338-1342).
- **Vertex colours in four bytes,** `XSDK_VERTEX_UBYTE4N`, x in the word's low byte (include/xsdk/shader.h), and
  strides of whole dwords (xenia:src/xenia/gpu/ucode.h:737-742).
- **D3D9's pixel centres:** xsdk sets `PA_SU_VTX_CNTL`'s `pix_center` to D3D's (src/gpu_setup.c:49, 285;
  xenia:src/xenia/gpu/registers.h:496), so a pixel's centre sits on whole coordinates and a quad that covers pixels
  exactly has its corners half a pixel up and left, as the resolve's rectangle has them (src/gpu.c:496).
- **Short-lived vertices guarded by fences:** the resolve's rectangles are slots reused only once the GPU has passed
  their fence, and refused inside recordings (src/gpu.c:479-516, `rectangle_slot`, `xsdk_gpu_rectangle`).
- **Shaders from GLSL** through xost, into a header (tools/build_shaders.py); libxsdk already carries generated shader
  microcode in its sources (src/gpu_resolve.c:123, src/gpu_setup.c:66).
- **Input:** `XamInputGetState` raw (docs/GUIDE.md, "Input and audio through the kernel"), and XAM's on-screen
  keyboard, `xsdk_xam_keyboard`, polled once a frame (include/xsdk/xam.h; milestone 51).

## The design

### Text in xsdk

The smallest API that covers an overlay and a menu's labels, in `include/xsdk/text.h` and `src/text.c`, in libxsdk:

```c
enum {
	// The font's cell, in pixels.
	XSDK_TEXT_CELL_WIDTH = 10,
	XSDK_TEXT_CELL_HEIGHT = 20,
};

// The font's atlas, the text shaders and a vertex ring in GPU memory; the GPU open.
NTSTATUS xsdk_text_open(void);
void xsdk_text_close(void);
// Queues utf8 with its first cell's top-left at (x, y) pixels from the viewport's, each cell scale times the font's,
// 1 to 8, in colour argb (0xAARRGGBB), blended over what's drawn; '\n' starts the next line at x.
void xsdk_text_draw(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8);
// The pixels utf8 takes at scale: its longest line's width, and its lines' height.
void xsdk_text_measure(const char *utf8, uint32_t scale, uint32_t *width, uint32_t *height);
```

- **One string, one draw.** `xsdk_text_draw` writes a quad a glyph into the ring, four vertices of a float2 position
  and a float2 atlas coordinate, and draws them as an indexed triangle list from a fixed index list, 6 indices a glyph.
  The colour is a pixel shader constant. Its corners sit half a pixel up and left (D3D9's centres, above), so with
  point sampling each font pixel lands on whole screen pixels, scale by scale.
- **Pixels, not clip space.** gpu_draw.c keeps the viewport's size, which `xsdk_gpu_set_target` and
  `xsdk_gpu_set_viewport` set, and text.c reads it through gpu_private.h for the vertex shader's scale and offset. No
  new public call.
- **The shaders:** `src/text.vert` and `src/text.frag`, a dozen lines between them: the position scaled into clip
  space, and the atlas's red as coverage times the colour. tools/build_shaders.py turns them into
  `src/text_shaders.h`, which is committed, so building libxsdk needs no glslang.
- **The ring:** 256 KB of GPU memory, 4,096 glyphs, in 64 KB blocks. Leaving a block queues a fence for it, and a
  block is written again only once the GPU has passed its fence, as the resolve's rectangles are. Text inside a
  recording or a tiled pass stops at an `assert`, as a recorded draw could outlive its block.
- **What it leaves set** for the draws after, as docs/GUIDE.md's tables say for every call: the shaders and their
  literals, vertex stream 0, texture slot 0, vertex and pixel constant 0, blending, depth and stencil off, culling off.
  It keeps the target, the viewport and the scissor, so a title clips text with `xsdk_gpu_set_scissor`.
- **Unicode:** UTF-8 in. A code point the font lacks, and a malformed byte, draw the font's default glyph, a box.
  Other control characters draw nothing.
- **Cost to titles that don't use it:** none. text.c is its own object in `libxsdk.a`, so the font and shaders link
  only into titles that call it.
- **No `printf` form:** a title formats with `snprintf`. An overlay of frame time is a few lines over
  `xsdk_present_stats` (include/xsdk/present.h).

### The font

**X11's misc-fixed 10x20,** Markus Kuhn's ISO 10646 set of X's fixed fonts, in the public domain: its BDF header
reads "Public domain font. Share and enjoy." (https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html). 10x20 is one of the
sizes with all of Latin, Greek and Cyrillic, and every size has WGL4's box drawing and block elements (same page).
- **What ships:** the BDF as published, in `assets/fonts/10x20.bdf`, and `tools/gen_font.py`, standard-library Python as
  tools/gen_splash.py is (CODE_GUIDELINES.md 2), which reads it into `src/text_font.h`: each glyph's 20 rows of 10
  bits, and a sorted table of code points.
- **Which glyphs:** ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, general punctuation, currency, arrows, box
  drawing, block elements and geometric shapes, as many of each as the font has: what the dashboard's European
  languages need (`xsdk_system_language`), about 900 glyphs, counted by the generator. About 36 KB in the title as
  bits.
- **The atlas:** `xsdk_text_open` expands the bits into a tiled 1024x256 R8 texture, 1,224 cells, 256 KB of GPU memory.
- **Scaling** by whole numbers only, point sampled, so text is never blurred.
- 10x20 gives 128 columns and 36 lines at 1280x720; 2 times gives a menu heading.

Not chosen (Decided, 3):
- **A TrueType font under the OFL, rasterised at build time:** smooth at any size, but host tools are standard-library
  Python, so it needs a TrueType rasteriser written in Python, or stb_truetype (public domain or MIT,
  imgui:imstb_truetype.h:6, 5049-5068) in a C host tool, a second exception to CODE_GUIDELINES.md 2. The OFL also asks
  that the licence ship with the font and limits renaming a converted copy.
- **stb_truetype on the console,** in libxsdk: a library added to the SDK for what ImGui already does for a title that
  wants TrueType.
- **Signed distance fields:** scalable from one atlas, at the cost of a filtered shader and soft small sizes.

### Dear ImGui

**The renderer: a backend on the GPU layer,** `backends/imgui_impl_xsdk.cpp` and `.h`, as imgui_impl_dx9 is on D3D9.
- **Vertices as ImGui writes them.** `ImDrawVert` is a float2 position, a float2 coordinate and an `ImU32` colour, 20
  bytes (imgui:imgui.h:3122-3127, 1.91.9b), with red in the colour's low byte (imgui.h:2874-2889): on the console the
  word holds 0xAABBGGRR, which `XSDK_VERTEX_UBYTE4N` reads as red, green, blue then alpha. Five dwords a vertex, so the
  buffers go to the GPU as ImGui fills them, through xsdk's `memcpy`, which writes GPU memory safely
  (docs/GUIDE.md, "Memory the GPU reads").
- **Indices** are ImGui's 16-bit ones, big-endian as the CPU writes them. A command's indices are copied to start on 4
  bytes, as the console has drawn lists only from there (docs/GUIDE.md, "Memory the GPU reads").
- **Large meshes:** `ImGuiBackendFlags_RendererHasVtxOffset`, the vertex stream pointed at each command's
  `VtxOffset`, 20 bytes a vertex, always 4-byte aligned (imgui:backends/imgui_impl_dx9.cpp:337).
- **State** as imgui_impl_dx9's (imgui_impl_dx9.cpp:109-136): source alpha over one minus source alpha for colour, one
  over one minus source alpha for alpha, the scissor on, no depth, no culling, a linear sampler. Its projection's half
  pixel (imgui_impl_dx9.cpp:146-149) holds for xsdk, which takes D3D9's centres.
- **Textures, ImGui 1.92 on:** a backend creates, updates and destroys the textures ImGui asks for each frame,
  `ImGuiBackendFlags_RendererHasTextures`, which ImGui means to require of every backend
  (https://github.com/ocornut/imgui/blob/master/docs/BACKENDS.md). The backend:
  - creates a tiled texture of one level from ImGui's RGBA32 pixels. ImGui writes those as `ImU32` words, as
    `IM_COL32` builds them (imgui:imgui_draw.cpp:2568-2572, 1.91.9b), so on the console a pixel is 0xAABBGGRR, not the
    bytes R, G, B, A: the backend takes them as `XSDK_FORMAT_A8R8G8B8` with red and blue swapped by
    `xsdk_texture_swizzle`, which milestone 29 proved;
  - updates a texture's changed rows only after the GPU has passed the last frame that used it, as uploads may not
    run while the GPU reads (include/xsdk/texture.h). Updates come when new glyphs are first drawn, so the wait is rare:
    stability over latency;
  - destroys one after the fence of the last frame that drew with it.
- **Buffers:** each frame's vertices and indices in the next of three slots, a slot reused once its fence has passed
  and grown when short, as the text ring is guarded. ImGui inside a recording stops at an `assert`.
- **Shaders:** a GLSL pair of a dozen lines, built by tools/build_shaders.py into a committed header, so a title that
  draws only ImGui needs no glslang.
- **Cost:** a CPU draw costs 0.289 us through xsdk against 0.966 through vulkan4xenon
  (vulkan4xenon:docs/ROADMAP.md:3055). ImGui's frame is tens to hundreds of draws, so either is small; what decides is
  who can use it.

Not chosen (Decided, 1): imgui_impl_vulkan through vulkan4xenon. It's 2,029 lines that need no change, and
vulkan4xenon lists what it uses: push constants, dynamic viewport and scissor, buffer-to-image copies and
`R8G8B8A8_UNORM` vertex input (vulkan4xenon:docs/SUPPORT.md:31, 200, 223, 750). But only a title built on Vulkan could
use it, so homebrew on the GPU layer and SDL3 titles couldn't. It's the way for Aurora's menus, as vulkan4xenon's
work (Left out).

**Input, in the same backend:**
- **Controller 1** through `XamInputGetState`, mapped to ImGui's gamepad keys as imgui_impl_win32 maps XInput's, which
  has the same layout (imgui:backends/imgui_impl_win32.cpp:395-446; xenia:src/xenia/hid/input.h:57-72), with
  `ImGuiBackendFlags_HasGamepad`, so a title turns on `ImGuiConfigFlags_NavEnableGamepad`. A title that reads its
  controllers itself passes a state in with `ImGui_ImplXsdk_ProcessGamepad`, which the frame's call uses too.
- **Display size and time:** the size `xsdk_present_open` gave, and the time from `clock_gettime`'s monotonic clock.
- **Text fields through XAM's keyboard** (Decided, 5): when a field takes focus to type, the backend opens
  `xsdk_xam_keyboard` with the field's text, polls it once a frame as docs/GUIDE.md says, and on accept replaces the
  field's text through ImGui's own key and character events, select all then the characters. A cancelled keyboard
  leaves the field as it was, whatever XAM returns for a cancel, which milestone 51 left open (docs/PARITY.md).
- **No mouse,** and ImGui draws no cursor.
- **Physical keyboards stay out.** `XamInputGetKeystrokeEx` (XAM 0x198, xenia:src/xenia/kernel/xam/xam_table.inc:205)
  is the call, but Xenia answers only for gamepads and refuses other device types
  (xenia:src/xenia/kernel/xam/xam_input.cc:155-180), so what a chatpad or USB keyboard gives is the console's to
  settle, with the hardware on hand.

**Where it lives: a fork, xenosworks/imgui-xenon** (Decided, 2), as SDL-xenon, mbedtls-xenon and curl-xenon are:
- Dear ImGui's latest release, v1.93.0 of 2026-10-14 on its releases page, on a `xenon` branch; upstream's files as
  they are, as ImGui builds for xsdk unchanged (Aurora, above).
- Added: the backend, its shaders and their header, `examples/example_xsdk/` with a demo and a check title built through
  cmake/xsdk.cmake, and `docs/README-xsdk.md`. The fork's style is ImGui's (CODE_GUIDELINES.md 14).
- A title adds ImGui's five sources and the backend to its build, as with any ImGui backend. Aurora could point its
  `FetchContent` at the fork, as it takes an `imgui` target already there (aurora-xenon:extern/CMakeLists.txt:229).
- ImGui is MIT; a title shipping it carries ImGui's notice, which the README says.

### A debug overlay of xsdk's own

None (Decided, 4). With `xsdk_text_draw` and `snprintf`, a title's overlay is a few lines, and it
knows what's worth showing; ImGui's windows and plots do the rest. docs/GUIDE.md shows a frame-time line as the example.

## What it takes

Steps 1 and 2 are one console run, 3 and 4 another, each run by the console agent with nobody needed.

1. **The font and its atlas,** in xsdk:
   - `assets/fonts/10x20.bdf`, and `tools/gen_font.py` writing `src/text_font.h`;
   - `xsdk_text_open` and `xsdk_text_close`: the atlas, the shaders from `src/text_shaders.h`, the ring and the index
     list, each freed in reverse;
   - on the console, text_glyphs (below) draws every block of the set and reads back each glyph's pixels.
2. **The text calls,** `xsdk_text_draw` and `xsdk_text_measure`, the viewport's size kept in gpu_draw.c, and
   `tests/test_text.c` with the rest of the checks below.
3. **imgui-xenon's renderer:** the fork made from v1.93.0, `ImGui_ImplXsdk_Init`, `ImGui_ImplXsdk_Shutdown` and
   `ImGui_ImplXsdk_RenderDrawData` into the target set now, with textures, buffers and state as above; the check
   title's drawing checks.
4. **imgui-xenon's input:** `ImGui_ImplXsdk_NewFrame(width, height)`, `ImGui_ImplXsdk_ProcessGamepad` and the
   keyboard; the check title's navigation and text-entry checks; and the demo, ImGui's demo window on the controller,
   which runs until you end it with Back or Xbox Home.
5. **Docs:** docs/GUIDE.md, "Text on screen", a table of the four calls as its other sections have them, and a pointer
   to imgui-xenon's README for ImGui; docs/PARITY.md's rows (below); docs/HARDWARE.md, both runs and the text's cost;
   THIRD_PARTY_NOTICES.md, the font's source, public domain, needing no notice in titles.

## Checks

Each is a line in its title's log, takes 0.5 to 1 s, and is broken on purpose once, through tests/breaks/ in xsdk and
the same way in the fork. Every pixel compare reads the resolved frame back as docs/GUIDE.md says.

**xsdk_test, `tests/test_text.c`:**

| Check | What passes | Broken on purpose, it fails |
|---|---|---|
| text_glyphs | A line with glyphs from every block of the set, at (37, 23), scale 1, opaque white on black: every pixel of every cell is the font's bit | the atlas's rows shifted by one; the corners without the half pixel, so cells land a pixel off |
| text_scale | The same line at scales 2 and 3: each font pixel a 2x2 and a 3x3 block | the scale applied to positions but not to cells |
| text_blend | 0x80FF0000 text over mid grey: its pixels blended to within 1 a channel, the others untouched | blending off |
| text_layout | "aé\nЖ─█" measures 30x40 at scale 1 and draws its second line at x, 20 below; U+4E00 and a lone 0xFF draw the default box | `\n` drawn as a glyph; measure counting bytes, so é counts twice |
| text_scissor | A string half outside the scissor: the pixels outside it untouched | xsdk_text_draw opening the scissor to the whole target |
| text_ring | 20,000 glyphs in one frame, more than the ring holds, then a marked string, over three frames: every string's first and last glyph where drawn | a block written again without waiting for its fence |
| text_cost | A full 1280x720 screen of glyphs, 4,608, queued in under 1 ms of CPU; its GPU time logged by `xsdk_gpu_write_clock` | a draw for each glyph |

**imgui-xenon's check title, `examples/example_xsdk/imgui_check.cpp`,** each frame built from fixed inputs, so a pinned
ImGui draws it the same every run:

| Check | What passes | Broken on purpose, it fails |
|---|---|---|
| imgui_window | A window at (100, 80), 300x200, square, unbordered, in a set colour: its edge pixels in, the ones beside them out, its inside exactly the colour | the projection without the half pixel |
| imgui_clip | A child region scrolled so a rectangle runs past its clip: nothing drawn outside the clip | clip rectangles ignored |
| imgui_image | `ImGui::Image` of a 4x4 xsdk texture of distinct colours at 64x64: each block's centre its texel | the command's texture ignored, the font's bound |
| imgui_font | Text at ImGui's default size, then at a size not drawn before: both show their glyphs' pixels in their boxes, white where the glyphs are solid | ImGui's pixels read as bytes, so glyphs come out as solid boxes; ImGui's texture updates ignored, so the new size draws nothing |
| imgui_large_mesh | One list of 17,000 rectangles, 68,000 vertices: the first and last where placed | `VtxOffset` ignored |
| imgui_nav | Controller states passed in: the D-pad down twice reaches the third button, and A presses it | the D-pad's up and down swapped |
| imgui_text_entry | A focused field given an accepted keyboard's text through the backend's completion holds it; a cancelled one leaves the field as it was | the accepted text dropped; a cancel taken as accepted |

The text entry check reaches the backend's keyboard completion through a function the check's build exposes, as XAM's
keyboard needs a person to accept it; milestone 51 checked that keyboard on the console by hand.

## Verification

On the console, through the console agent:
- xsdk_test with its new checks, passing; each break in the table run once and failing its check and no other;
- imgui_check passing, and each of its breaks failing its check;
- vulkan4xenon_test and vkbench once each, unchanged, as gpu_draw.c now keeps the viewport's size.

In Xenia, as a smoke check, with `--readback_resolve=full`: the text and ImGui checks are expected to pass there; any
that can't gets its line in CLAUDE.md.

You can try the demo when you like; the milestone closes on the checks, not on a look.

## Done when

- xsdk_test's text checks and imgui_check's checks pass on the console, and every break fails its own check.
- vulkan4xenon_test and vkbench pass unchanged.
- docs/HARDWARE.md records both runs and text_cost's times; docs/GUIDE.md has "Text on screen"; docs/PARITY.md has
  the rows below; imgui-xenon's README says how a title adds ImGui.
- imgui-xenon is pushed only when you ask, after its run passes.

**docs/PARITY.md's rows:**
- New, a section "Text and UI" after "Video output": "Text on screen, a public-domain 10x20 bitmap font of about 900
  glyphs, Latin, Greek and Cyrillic, scaled by whole numbers", Yes, `xsdk_text_*` (`text.h`), `tests/test_text.c`,
  missing proportional and TrueType fonts and CJK, which ImGui gives a title that brings its own TrueType.
- New, under "Libraries ported to xsdk": "Dear ImGui (imgui-xenon)", Yes, windows and widgets on the GPU layer, driven
  by controller 1, text fields through XAM's keyboard, `examples/example_xsdk/imgui_check.cpp` there, missing physical
  keyboards and ImGui through vulkan4xenon.
- "On-screen keyboard, message boxes, sign-in prompt": How gains "ImGui's text fields open it (imgui-xenon)".
- Unchanged: "Chatpad and USB keyboard", No; "Controllers", Partial, as the backend reads `XamInputGetState` raw.
- The page's dates and its "Done since" line gain milestone 57.

## Left out

- **An overlay of xsdk's own** (Decided, 4).
- **In xsdk's text:** TrueType and proportional fonts, kerning, scales between whole numbers, rotation, combining marks,
  right-to-left scripts and CJK. ImGui draws a title's TrueType, CJK included.
- **Text and ImGui inside recordings and tiled passes,** which stop at an `assert`.
- **Chatpads and USB keyboards** (`XamInputGetKeystrokeEx`), above.
- **ImGui through vulkan4xenon, and Dusklight's menus:** Aurora's vulkan4xenon backend drops ImGui's draw data
  (aurora-xenon:lib/vulkan4xenon/encoding.cpp:1281-1282); drawing it there through imgui_impl_vulkan is vulkan4xenon's
  and Aurora's work, after this.
- **ImGui's docking branch and multiple viewports,** until a title asks; FreeType in ImGui.
- **An SDL3 render driver,** docs/PARITY.md's seventh gap: ImGui doesn't give SDL one.

## Decided

On 2026-10-21:
1. **ImGui's renderer:** a backend on xsdk's GPU layer, about 500 lines, as imgui_impl_dx9 is on D3D9. Every xsdk
   title can use it, at a third of the CPU a draw, and it's D3D9's shape. imgui_impl_vulkan unchanged through
   vulkan4xenon, and both, were the others; it serves only titles on Vulkan, suits Aurora's menus, and can be
   vulkan4xenon's own milestone.
2. **ImGui lives in a fork,** xenosworks/imgui-xenon, as the other ports are: ImGui's latest release on a `xenon`
   branch, the backend, its check and demo added, and xsdk stays C. You create the repository; it's pushed to only
   when you ask, after its run has passed. ImGui builds unchanged, so the fork only adds files, and taking a new
   release is a merge of upstream's. ImGui vendored in xsdk under `third_party/imgui`, and the backend alone in xsdk
   with ImGui fetched by the check's build, were the others, both putting C++ in xsdk.
3. **The font:** X11's misc-fixed 10x20 alone, public domain, about 900 glyphs, at whole-number scales: the plainest
   that's sharp on a television, and ImGui covers smooth and large text. 6x13 beside it for dense overlays, and a
   TrueType under the OFL rasterised at build time, were the others.
4. **No debug overlay of xsdk's own:** titles draw their own with the text calls or ImGui, and docs/GUIDE.md shows a
   frame-time line. `xsdk_overlay_draw`, with frame time, the ring's waits and the log's last lines kept by src/log.c,
   was the other way; it can come when a porter asks, as the log already reaches the PC (milestone 53).
5. **ImGui's text fields** are typed into through XAM's on-screen keyboard, which milestone 51 brought: a shop's
   search and a homebrew's settings need it, and it's a small piece over milestone 51's call. Fields that only show
   text, a title opening the keyboard itself, were the other way.

## Size

- **xsdk:** about 700 lines: text.c, its header, its two shaders, gen_font.py, test_text.c, the breaks and the docs; the
  font's BDF and two generated headers besides.
- **imgui-xenon:** about 1,000 lines: the backend, its shaders, the check, the demo, the CMake and the README.
- **Console:** two runs by the console agent, each with its breaks, plus vulkan4xenon_test and vkbench once.
- **From you:** creating xenosworks/imgui-xenon. Nothing needs you at the console.

## Risks

- **ImGui's textures are new upstream:** 1.92 changed how backends get them, and BACKENDS.md calls the old way
  obsolete. The backend is written to the pinned release, and each new release reruns imgui_check.
- **Big-endian data:** ImGui fills its pixels and colours as words (above). The font and window checks catch a wrong
  reading; the backend reads no other byte data of ImGui's.
- **A wait for new glyphs** can stall a frame when ImGui first draws a new size. Updates are rare, and it's the stable
  choice.
- **Pixel-exact text** rests on D3D9's centres and point sampling; text_glyphs and imgui_window settle it on the
  console, their half-pixel breaks failing.

## Done (2026-10-21)

[The console's runs](../../results/2026-10-21-milestone-57/README.md) passed: xsdk_test with only its controller
checks failing, every text check pixel-exact, imgui-xenon's imgui_check 7 of 7, and each of their 18 breaks failing
its own checks. Two things changed on the way. Text first queued a full screen in 1,308 us, over its 1 ms; 16-byte VMX
stores and atlas coordinates worked out once brought it to 613. And Xenos's linear filter proved not exact at a
texel's centre, so ImGui's font check samples point while titles keep linear filtering (docs/HARDWARE.md, "Milestone
57 on the console").
