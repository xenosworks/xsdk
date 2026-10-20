#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "target_shaders.h"
#include "tests.h"
#include "texture_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsc_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"
#include "xsdk/xsc.h"

// Tiled passes (docs/scope/44-tiling-and-msaa.md, steps 2 and 3). Each tiled frame is checked against the same draws
// made untiled into a target small enough to fit, moved by the viewport so its rows line up, every pixel of every
// target equal:
// - 1280x720, one 8_8_8_8 target with depth, in 2 and 3 bands and with depth whole in 3, from tile 0 and at eDRAM's
//   top: a gl_FragCoord gradient from xost compiled on the console, triangles whose shallow and steep edges cross the
//   bands, near and far quads over a depth clear tested against the hierarchical buffer, and a triangle given band 0's
//   rows that reaches every band;
// - the same at 2 samples in 2 bands and with depth whole in 3, against 1280x512 pieces, and at 4 samples in 3 bands,
//   against 1280x256 pieces, the bands as few as fit and the gradient a flat tint, as a piece's gl_FragCoord starts at
//   its own top; and at 4 samples with its depth resolved too, listed after the colour;
// - a pass drawing its own texture back first, as vulkan4xenon loads an attachment, band 0's clear leaving it alone;
// - two passes laid out the same back to back, clearing to different colours, the second's band 0 its own clear;
// - four targets of four formats at 1280x720 with depth, in as many bands as they need, each read by value;
// - 2048x2048 depth alone in bands, its depth resolved a band at a time and each texel checked, then sampled by a quad
//   in a tiled pass of its own, each texel checked again.
// Each frame shows for a capture, each variant in its own colours.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BIG = 2048,
	FENCE_TIMEOUT_MS = 2000,
	COMMAND_DWORDS = 4096,
	FRAMES = 2,
	// Long enough for a capture's frames, each check staying under a second.
	VARIANT_SHOW_MS = TEST_SHOW_MS / 2,
	QUAD_VERTICES = 6,
	TRIANGLE_VERTICES = 3,
	CROSSING_TRIANGLES = 4,
	TRIANGLES = CROSSING_TRIANGLES + 1,
	SQUARE_FLOATS = 4,
	COLORED_FLOATS = 8,
	COLOR_OFFSET_BYTES = 16,
	TEXTURED_FLOATS = 6,
	TEXCOORD_OFFSET_BYTES = 16,
	// The texture shaders sample fetch constant 0 (tools/gen_texture_shaders.py).
	TEXTURE_SLOT = 0,
	CHANNELS = 4,
	// The four targets' bands, and the reference's pieces: 6 tiles a 16 rows of 1280, so 256 rows a band.
	FOUR_ROWS = 256,
	FOUR_DRAWS = 4,
	// 1280x512 at 2 samples with depth takes eDRAM's 2048 tiles.
	HALF_ROWS = 512,
	CHUNK_ROWS = 32,
	MAX_TEXEL_BYTES = 8,
	MAX_MISSES_LOGGED = 4,
	DEPTH_24_MAX = 0xFFFFFF,
	// As far as tests/test_depth.c lets a resolved depth and its fetch be.
	DEPTH_24_TOLERANCE = 2,
	CHANNEL_TOLERANCE = 1,
	BIG_RECTS = 4,
	LABEL_BYTES = 160,
	// The square, the triangles, and quads over the 2048x2048 target, the screen from it, and a 1280x720 frame.
	VERTEX_BUFFERS = 5,
};

// Rectangles in pixels, right and bottom exclusive, and their depth.
typedef struct {
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	float depth;
} Rect;

// One 8_8_8_8 target at 1280x720: its bands, its depth's layout, and the colours that mark it on screen.
typedef struct {
	const char *what;
	uint32_t band_rows;
	bool depth_whole;
	uint32_t clear_argb;
	uint32_t tint_argb;
	uint32_t near_argb;
	uint32_t far_argb;
	bool depth_top;
	uint32_t samples;
	// The bands as few as fit, which band_rows is then what they come to; else band_rows asked for.
	bool fewest;
	// Its depth resolved too, into a D24S8 texture, listed after the colour as vulkan4xenon lists a stored depth.
	bool depth_resolved;
} Variant;

static const Variant variants[] = {
	{ "1280x720 in 2 bands of 384 rows", 384, false, 0xFF202040, 0xFF000060, 0xFF40E040, 0xFFE04040, false, 1, false,
		false },
	{ "1280x720 in 3 bands of 256 rows", 256, false, 0xFF402020, 0xFF0000C0, 0xFF40E0E0, 0xFFE0A040, false, 1, false,
		false },
	{ "1280x720 with depth whole in 3 bands of 256 rows", 256, true, 0xFF204020, 0xFF0000FF, 0xFFE040E0, 0xFF4040E0,
		false, 1, false, false },
	{ "1280x720 with depth whole at eDRAM's top in 3 bands of 256 rows", 256, true, 0xFF203838, 0xFF00A0A0, 0xFFA0E040,
		0xFFE0E0E0, true, 1, false, false },
};

// Hierarchical depth on in the bands of all but the second, 384 rows at 2 samples and 256 at 4, and in their
// references' pieces (src/gpu_draw.c, xsdk_gpu_hiz_fits).
static const Variant sampled_variants[] = {
	{ "1280x720 at 2 samples in 2 bands of 384 rows", 384, false, 0xFF283048, 0xFF305078, 0xFF60E0A0, 0xFFE08060, false,
		2, true, false },
	{ "1280x720 at 2 samples with depth whole in 3 bands of 256 rows", 256, true, 0xFF482830, 0xFF784060, 0xFFA0E060,
		0xFF6080E0, false, 2, true, false },
	{ "1280x720 at 4 samples in 3 bands of 256 rows", 256, false, 0xFF304828, 0xFF507830, 0xFFE0A0E0, 0xFF60E0E0, false,
		4, true, false },
	{ "1280x720 at 4 samples in 3 bands of 256 rows, its depth resolved too", 256, false, 0xFF402838, 0xFF784870,
		0xFFE0E060, 0xFF60A0E0, false, 4, true, true },
};

// The back-to-back passes' clears.
static const uint32_t back_to_back_argb[2] = { 0xFF305070, 0xFF804020 };

// Corners on whole pixels, x a multiple of 5 and y of 45, so each clip coordinate is exact and the viewport moved by a
// band's top places it exactly where the window offset does.
static const Rect near_band_0 = { 100, 45, 500, 225, 0.25f };
static const Rect near_across = { 700, 270, 1000, 630, 0.3125f };
static const Rect far_across = { 300, 45, 800, 675, 0.5f };
static const Rect whole = { 0, 0, WIDTH, HEIGHT, 0.0f };

// Shallow and steep edges across rows 256, 384 and 512, then a triangle reaching every band.
static const uint32_t triangle_corners[TRIANGLES][TRIANGLE_VERTICES][2] = {
	{ { 0, 225 }, { 1280, 315 }, { 0, 450 } },
	{ { 600, 0 }, { 680, 720 }, { 560, 675 } },
	{ { 100, 495 }, { 1200, 540 }, { 1250, 585 } },
	{ { 900, 270 }, { 1010, 630 }, { 880, 630 } },
	{ { 200, 90 }, { 400, 630 }, { 150, 585 } },
};
static const uint32_t triangle_argb[TRIANGLES] = { 0xFFE06020, 0xFF20E060, 0xFF6020E0, 0xFFE0E020, 0xFFE020E0 };

// The four targets: formats, the textures they resolve into, their clears, and each draw's colour for each.
static const uint32_t four_formats[XSDK_COLOR_TARGETS] = { XSDK_TARGET_8_8_8_8, XSDK_TARGET_2_10_10_10,
	XSDK_TARGET_16_16_16_16_FLOAT, XSDK_TARGET_32_FLOAT };
static const uint32_t four_textures[XSDK_COLOR_TARGETS] = { XSDK_FORMAT_B8G8R8A8, XSDK_FORMAT_A2B10G10R10,
	XSDK_FORMAT_R16G16B16A16F, XSDK_FORMAT_R32F };
static const uint32_t four_texel_bytes[XSDK_COLOR_TARGETS] = { 4, 4, 8, 4 };
static const float four_clears[XSDK_COLOR_TARGETS][CHANNELS] = {
	{ 0.1f, 0.15f, 0.3f, 1.0f },
	{ 0.2f, 0.2f, 0.2f, 1.0f },
	{ 0.25f, 0.5f, 0.75f, 1.0f },
	{ 0.125f, 0.0f, 0.0f, 0.0f },
};
// The triangles, then near_band_0, near_across and far_across.
static const float four_colors[FOUR_DRAWS][XSDK_COLOR_TARGETS][CHANNELS] = {
	{ { 0.9f, 0.5f, 0.1f, 1.0f }, { 0.1f, 0.6f, 0.9f, 1.0f }, { 2.5f, 0.75f, 1.5f, 1.0f }, { 7.25f, 0, 0, 0 } },
	{ { 0.2f, 0.9f, 0.3f, 1.0f }, { 0.8f, 0.3f, 0.6f, 1.0f / 3 }, { 0.5f, 3.0f, 0.25f, 2.0f }, { -3.5f, 0, 0, 0 } },
	{ { 0.7f, 0.2f, 0.9f, 1.0f }, { 0.4f, 0.9f, 0.1f, 1.0f }, { 1.0f, 1.5f, 3.5f, 0.5f }, { 42.0f, 0, 0, 0 } },
	{ { 0.9f, 0.8f, 0.2f, 1.0f }, { 0.6f, 0.6f, 0.6f, 2.0f / 3 }, { 0.125f, 0.25f, 4.0f, 1.0f }, { 1234.5f, 0, 0, 0 } },
};

// 2048x2048 depth alone: everything, a band across row 1024, a quad partly behind it, and a near square on the row.
static const Rect big_rects[BIG_RECTS] = {
	{ 0, 0, BIG, BIG, 0.75f },
	{ 100, 900, 1900, 1200, 0.375f },
	{ 500, 100, 1500, 1900, 0.625f },
	{ 1000, 1000, 1100, 1100, 0.25f },
};

static const XsdkVertexElement square_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static const XsdkVertexElement colored_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static const XsdkVertexElement textured_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = TEXCOORD_OFFSET_BYTES },
};

static const float square[QUAD_VERTICES][SQUARE_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

typedef struct {
	XsdkShader rect_vertex;
	XsdkShader colored_vertex;
	XsdkShader textured_vertex;
	XsdkShader flat_pixel;
	XsdkShader four_pixel;
	XsdkShader textured_pixel;
	XsdkShader gradient_pixel;
	XsdkXscShader gradient;
	uint32_t square;
	uint32_t triangles;
	uint32_t big_screen;
	uint32_t show_screen;
	uint32_t frame_screen;
	XsdkDepthStencilState less;
	XsdkDepthStencilState off;
	XsdkCommandBuffer commands;
	XsdkTexture frames[FRAMES];
	XsdkTexture reference;
	// The reference's pieces at 2 samples, 512 rows each; at 4 they're pieces[0], 256 rows.
	XsdkTexture half;
	XsdkTexture four[XSDK_COLOR_TARGETS];
	XsdkTexture pieces[XSDK_COLOR_TARGETS];
	XsdkTexture big_depth;
	XsdkTexture big_sampled;
	// The 1280x720 depth a variant resolves.
	XsdkTexture depth;
	bool has_display;
	bool shown;
	uint32_t frame;
} Scene;

static uint8_t rows_a[WIDTH * CHUNK_ROWS * MAX_TEXEL_BYTES];
static uint8_t rows_b[WIDTH * CHUNK_ROWS * MAX_TEXEL_BYTES];

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

static void rgba_of(uint32_t argb, float rgba[CHANNELS])
{
	rgba[0] = unorm(argb >> 16);
	rgba[1] = unorm(argb >> 8);
	rgba[2] = unorm(argb);
	rgba[3] = unorm(argb >> 24);
}

static float clip_x(uint32_t x, uint32_t width)
{
	return (float)x / (width / 2.0f) - 1.0f;
}

static float clip_y(uint32_t y, uint32_t height)
{
	return 1.0f - (float)y / (height / 2.0f);
}

// The constant vertex shader places the unit square at c0 * position + c1, the depth in c1.z, coloured by c2
// (tools/gen_constant_shaders.py).
static void place_square(float left, float top, float right, float bottom, float depth, uint32_t argb)
{
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// A rectangle of the 1280x720 target, its corners on whole pixels.
static void draw_rect(const Rect *r, uint32_t argb)
{
	place_square(
		clip_x(r->x0, WIDTH), clip_y(r->y0, HEIGHT), clip_x(r->x1, WIDTH), clip_y(r->y1, HEIGHT), r->depth, argb);
}

// A rectangle of the 2048x2048 target, edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837), so it covers
// x0 to x1 - 1 and y0 to y1 - 1 as tests/test_hiz.c models it.
static void draw_big_rect(const Rect *r)
{
	const float half = 1.0f / BIG;
	place_square(clip_x(r->x0, BIG) - half, clip_y(r->y0, BIG) + half, clip_x(r->x1, BIG) - half,
		clip_y(r->y1, BIG) + half, r->depth, 0xFFFFFFFF);
}

// xost's literals for the gradient, as a title sets a shader's needs (tests/test_raster.c).
static void set_gradient_literals(const XsdkXscShader *gradient)
{
	for (size_t i = 0; i < gradient->need_count; i++) {
		const XsdkXscNeed *const need = &gradient->needs[i];
		const float values[4] = { (float)need->value[0], (float)need->value[1], (float)need->value[2],
			(float)need->value[3] };
		if (need->kind == XSDK_XSC_LITERAL)
			xsdk_gpu_set_pixel_constants(need->reg, values, 1);
	}
}

// The single target's draws; untiled, into rows top on of a target `rows` tall, band 0's triangle is scissored to band
// 0's rows, where a tiled pass predicates it.
static void draw_single(const Scene *s, const Variant *v, bool tiled, uint32_t top, uint32_t rows)
{
	xsdk_gpu_set_shaders(&s->rect_vertex, v->samples == 1 ? &s->gradient_pixel : &s->flat_pixel);
	xsdk_gpu_set_vertices(0, s->square, sizeof square);
	if (v->samples == 1)
		set_gradient_literals(&s->gradient);
	draw_rect(&whole, v->tint_argb);
	xsdk_gpu_set_shaders(&s->colored_vertex, &s->flat_pixel);
	xsdk_gpu_set_vertices(0, s->triangles, TRIANGLES * TRIANGLE_VERTICES * COLORED_FLOATS * 4);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, CROSSING_TRIANGLES * TRIANGLE_VERTICES);
	xsdk_gpu_set_shaders(&s->rect_vertex, &s->flat_pixel);
	xsdk_gpu_set_vertices(0, s->square, sizeof square);
	xsdk_gpu_set_depth_stencil(&s->less);
	draw_rect(&near_band_0, v->near_argb);
	draw_rect(&near_across, v->near_argb);
	draw_rect(&far_across, v->far_argb);
	xsdk_gpu_set_depth_stencil(&s->off);
	xsdk_gpu_set_shaders(&s->colored_vertex, &s->flat_pixel);
	xsdk_gpu_set_vertices(0, s->triangles + CROSSING_TRIANGLES * TRIANGLE_VERTICES * COLORED_FLOATS * 4,
		TRIANGLE_VERTICES * COLORED_FLOATS * 4);
	if (tiled)
		xsdk_gpu_set_draw_rows(0, v->band_rows);
	else if (top < v->band_rows)
		xsdk_gpu_set_scissor(0, 0, WIDTH, v->band_rows - top);
	if (tiled || top < v->band_rows)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, TRIANGLE_VERTICES);
	if (tiled)
		xsdk_gpu_set_draw_rows(0, HEIGHT);
	else
		xsdk_gpu_set_scissor(0, 0, WIDTH, rows);
}

// The four targets' draws, each with its colour for each target from c0 to c3 (tools/gen_target_shaders.py).
static void draw_four(const Scene *s)
{
	static const Rect *const rects[FOUR_DRAWS - 1] = { &near_band_0, &near_across, &far_across };
	xsdk_gpu_set_shaders(&s->colored_vertex, &s->four_pixel);
	xsdk_gpu_set_vertices(0, s->triangles, TRIANGLES * TRIANGLE_VERTICES * COLORED_FLOATS * 4);
	xsdk_gpu_set_pixel_constants(0, &four_colors[0][0][0], XSDK_COLOR_TARGETS);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, CROSSING_TRIANGLES * TRIANGLE_VERTICES);
	xsdk_gpu_set_shaders(&s->rect_vertex, &s->four_pixel);
	xsdk_gpu_set_vertices(0, s->square, sizeof square);
	xsdk_gpu_set_depth_stencil(&s->less);
	for (uint32_t i = 0; i < FOUR_DRAWS - 1; i++) {
		xsdk_gpu_set_pixel_constants(0, &four_colors[i + 1][0][0], XSDK_COLOR_TARGETS);
		draw_rect(rects[i], 0);
	}
	xsdk_gpu_set_depth_stencil(&s->off);
}

static bool finished(const char *what)
{
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	if (!landed)
		xsdk_log_write("tiles: %s: the fence didn't land", what);
	return landed;
}

static uint32_t first_word(const uint8_t *texel)
{
	uint32_t word;
	memcpy(&word, texel, sizeof word);
	return word;
}

// The texels of rows a_top on of a that differ from rows b_top on of b, rows of each, 1280 texels wide; the first few
// logged.
static uint32_t count_differing(const char *what, const XsdkTexture *a, uint32_t a_top, const XsdkTexture *b,
	uint32_t b_top, uint32_t rows, uint32_t texel_bytes)
{
	const uint32_t row_bytes = WIDTH * texel_bytes;
	uint32_t differing = 0;
	for (uint32_t done = 0; done < rows; done += CHUNK_ROWS) {
		const uint32_t count = rows - done < CHUNK_ROWS ? rows - done : CHUNK_ROWS;
		xsdk_texture_download_rows(a, 0, a_top + done, count, rows_a, row_bytes);
		xsdk_texture_download_rows(b, 0, b_top + done, count, rows_b, row_bytes);
		for (uint32_t i = 0; i < count * WIDTH; i++) {
			const uint8_t *const got = &rows_a[i * texel_bytes];
			const uint8_t *const want = &rows_b[i * texel_bytes];
			if (!memcmp(got, want, texel_bytes))
				continue;
			if (differing++ < MAX_MISSES_LOGGED)
				xsdk_log_write("tiles: %s: (%u, %u) starts %08X tiled, %08X untiled", what, (unsigned)(i % WIDTH),
					(unsigned)(a_top + done + i / WIDTH), (unsigned)first_word(got), (unsigned)first_word(want));
		}
	}
	return differing;
}

static void show(Scene *s, const XsdkTexture *frame)
{
	if (!s->has_display)
		return;
	s->shown = s->shown && NT_SUCCESS(xsdk_display_show(frame->address, frame->pitch));
	(void)xsdk_thread_sleep(VARIANT_SHOW_MS);
}

// Depth whole is the title's to clear, here by a target with it where the pass keeps it, at tile 0 with the colour past
// it or at the top with the colour at 0; at 2 samples, where the colour doesn't fit beside it, on its tiles, clearing
// depth alone.
static void clear_depth_whole(const Variant *v, const XsdkTexture *frame)
{
	const uint32_t depth_tiles = xsdk_gpu_target_tiles(WIDTH, HEIGHT);
	if (v->samples > 1)
		xsdk_gpu_set_target_samples_at(WIDTH, HEIGHT, v->samples, 0, 0);
	else if (v->depth_top)
		xsdk_gpu_set_target_at(WIDTH, HEIGHT, 0, XSDK_EDRAM_TILES - depth_tiles);
	else
		xsdk_gpu_set_target_at(WIDTH, HEIGHT, depth_tiles, 0);
	xsdk_gpu_set_resolve_clears(v->samples == 1, true);
	xsdk_gpu_resolve_texture(frame, v->clear_argb);
	xsdk_gpu_set_resolve_clears(true, true);
}

// The tiled frame, one target's frame of 1280x720 whole.
static bool draw_tiled(Scene *s, const Variant *v, const XsdkTexture *frame)
{
	const XsdkTiling tiling = { .width = WIDTH,
		.height = HEIGHT,
		.samples = v->samples,
		.targets = { { .format = XSDK_TARGET_8_8_8_8 } },
		.count = 1,
		.depth = true,
		.depth_whole = v->depth_whole,
		.band_rows = v->fewest ? 0 : v->band_rows,
		.depth_top = v->depth_top };
	XsdkTileResolve resolves[2] = { { .target = 0, .texture = frame },
		{ .target = XSDK_TILE_DEPTH, .texture = &s->depth } };
	rgba_of(v->clear_argb, resolves[0].clear_rgba);
	if (v->depth_whole)
		clear_depth_whole(v, frame);
	if (!NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &s->commands)))
		return false;
	draw_single(s, v, true, 0, HEIGHT);
	return NT_SUCCESS(xsdk_gpu_end_tiles(resolves, v->depth_resolved ? 2 : 1));
}

// The reference's rows from top, untiled into a target as tall as the piece at the variant's samples, its depth from
// tile 0 and its colour after, the viewport moved up by top.
static bool draw_piece(const Scene *s, const Variant *v, const XsdkTexture *piece, uint32_t top)
{
	const XsdkColorTarget target = { .format = XSDK_TARGET_8_8_8_8,
		.tile = xsdk_gpu_target_sample_tiles(WIDTH, piece->height, v->samples) };
	float clear[CHANNELS];
	rgba_of(v->clear_argb, clear);
	xsdk_gpu_set_targets_samples(WIDTH, piece->height, v->samples, &target, 1, 0);
	xsdk_gpu_resolve_target(0, piece, clear);
	xsdk_gpu_set_targets_samples(WIDTH, piece->height, v->samples, &target, 1, 0);
	xsdk_gpu_set_viewport(0.0f, -(float)top, WIDTH, HEIGHT, 0.0f, 1.0f);
	draw_single(s, v, false, top, piece->height);
	xsdk_gpu_resolve_target(0, piece, clear);
	return finished(v->what);
}

// The tiled frame against the reference, which fits eDRAM whole at 1 sample, and in pieces of 512 rows at 2 samples and
// 256 at 4.
static void check_variant(Scene *s, const Variant *v)
{
	const XsdkTexture *const frame = &s->frames[s->frame++ % FRAMES];
	const XsdkTexture *const piece = v->samples == 1 ? &s->reference : v->samples == 2 ? &s->half : &s->pieces[0];
	bool drawn = draw_tiled(s, v, frame);
	uint32_t differing = 0;
	for (uint32_t top = 0; drawn && top < HEIGHT; top += piece->height) {
		const uint32_t rows = HEIGHT - top < piece->height ? HEIGHT - top : piece->height;
		drawn = draw_piece(s, v, piece, top);
		differing += drawn ? count_differing(v->what, frame, top, piece, 0, rows, 4) : 0;
	}
	xsdk_log_write(
		"tiles: %s: %u of %u pixels differ from untiled", v->what, (unsigned)differing, (unsigned)(WIDTH * HEIGHT));
	char name[LABEL_BYTES];
	snprintf(name, sizeof name, "tiles: %s, every pixel as untiled", v->what);
	check(name, drawn && differing == 0);
	if (drawn)
		show(s, frame);
}

// The four targets after the 1280x720 depth, as a tiled pass places them, here for the reference's 256-row pieces.
static void place_four(XsdkColorTarget targets[XSDK_COLOR_TARGETS])
{
	uint32_t tile = xsdk_gpu_target_tiles(WIDTH, FOUR_ROWS);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		targets[i] = (XsdkColorTarget){ .format = four_formats[i], .tile = tile };
		tile += xsdk_gpu_color_target_tiles(WIDTH, FOUR_ROWS, four_formats[i]);
	}
}

// Rows top on of the reference, drawn into 256-row targets with the viewport moved up by top.
static bool draw_four_piece(const Scene *s, uint32_t top)
{
	XsdkColorTarget targets[XSDK_COLOR_TARGETS];
	place_four(targets);
	xsdk_gpu_set_targets(WIDTH, FOUR_ROWS, targets, XSDK_COLOR_TARGETS, 0);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_gpu_resolve_target(i, &s->pieces[i], four_clears[i]);
	xsdk_gpu_set_targets(WIDTH, FOUR_ROWS, targets, XSDK_COLOR_TARGETS, 0);
	xsdk_gpu_set_viewport(0.0f, -(float)top, WIDTH, HEIGHT, 0.0f, 1.0f);
	draw_four(s);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_gpu_resolve_target(i, &s->pieces[i], four_clears[i]);
	return finished("four targets' reference");
}

static bool draw_four_tiled(Scene *s)
{
	XsdkTiling tiling = { .width = WIDTH, .height = HEIGHT, .samples = 1, .count = XSDK_COLOR_TARGETS, .depth = true };
	XsdkTileResolve resolves[XSDK_COLOR_TARGETS];
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		tiling.targets[i].format = four_formats[i];
		resolves[i] = (XsdkTileResolve){ .target = i, .texture = &s->four[i] };
		memcpy(resolves[i].clear_rgba, four_clears[i], sizeof resolves[i].clear_rgba);
	}
	const uint32_t bands = xsdk_gpu_tiling_bands(&tiling);
	xsdk_log_write("tiles: four targets at 1280x720 with depth take %u bands", (unsigned)bands);
	if (bands != (HEIGHT + FOUR_ROWS - 1) / FOUR_ROWS || !NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &s->commands)))
		return false;
	draw_four(s);
	return NT_SUCCESS(xsdk_gpu_end_tiles(resolves, XSDK_COLOR_TARGETS)) && finished("four targets tiled");
}

// Each target's frame against the reference's pieces, a band's rows at a time.
static void check_four(Scene *s)
{
	static const char *const names[XSDK_COLOR_TARGETS] = { "8_8_8_8", "2_10_10_10", "16_16_16_16_FLOAT", "32_FLOAT" };
	bool drawn = draw_four_tiled(s);
	uint32_t differing[XSDK_COLOR_TARGETS] = { 0 };
	for (uint32_t top = 0; drawn && top < HEIGHT; top += FOUR_ROWS) {
		drawn = draw_four_piece(s, top);
		const uint32_t rows = HEIGHT - top < FOUR_ROWS ? HEIGHT - top : FOUR_ROWS;
		for (uint32_t i = 0; drawn && i < XSDK_COLOR_TARGETS; i++)
			differing[i] += count_differing(names[i], &s->four[i], top, &s->pieces[i], 0, rows, four_texel_bytes[i]);
	}
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		char name[LABEL_BYTES];
		xsdk_log_write("tiles: four targets, target %u, %s: %u of %u texels differ from untiled", (unsigned)i, names[i],
			(unsigned)differing[i], (unsigned)(WIDTH * HEIGHT));
		snprintf(name, sizeof name, "tiles: four targets at 1280x720 in bands, target %u, %s, every texel as untiled",
			(unsigned)i, names[i]);
		check(name, drawn && differing[i] == 0);
	}
	if (drawn)
		show(s, &s->four[0]);
}

static float big_depth_at(uint32_t x, uint32_t y)
{
	float depth = 1.0f;
	for (uint32_t i = 0; i < BIG_RECTS; i++) {
		const Rect *const r = &big_rects[i];
		if (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1 && r->depth < depth)
			depth = r->depth;
	}
	return depth;
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// Every texel's word: the depth in its top 24 bits, stencil 0 below.
static uint32_t count_wrong_depth(const XsdkTexture *texture)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < BIG; y++) {
		for (uint32_t x = 0; x < BIG; x++) {
			const uint32_t word = texture->texels[tiled_index(x, y, texture->pitch)];
			const uint32_t want = (uint32_t)(big_depth_at(x, y) * (double)DEPTH_24_MAX + 0.5);
			if (difference(word >> 8, want) <= DEPTH_24_TOLERANCE && (word & 0xFF) == 0)
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("tiles: 2048x2048 depth: (%u, %u) holds %08X, for depth %06X", (unsigned)x, (unsigned)y,
					(unsigned)word, (unsigned)want);
		}
	}
	return wrong;
}

// Every texel's red the depth sampled, green and blue 0 and alpha 1; a resolve stores B, G, R, A (docs/HARDWARE.md).
static uint32_t count_wrong_samples(const XsdkTexture *texture)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < BIG; y++) {
		for (uint32_t x = 0; x < BIG; x++) {
			const uint32_t stored = texture->texels[tiled_index(x, y, texture->pitch)];
			const uint32_t red = (uint32_t)(big_depth_at(x, y) * 255.0f + 0.5f);
			if (difference(stored >> 8 & 0xFF, red) <= CHANNEL_TOLERANCE && (stored >> 16) == 0 &&
				(stored & 0xFF) == 0xFF)
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("tiles: 2048x2048 sampled: (%u, %u) stored %08X, for red %u", (unsigned)x, (unsigned)y,
					(unsigned)stored, (unsigned)red);
		}
	}
	return wrong;
}

static bool draw_big_depth(Scene *s)
{
	const XsdkTiling tiling = { .width = BIG, .height = BIG, .samples = 1, .depth = true };
	const XsdkTileResolve resolve = { .target = XSDK_TILE_DEPTH, .texture = &s->big_depth };
	xsdk_log_write("tiles: 2048x2048 depth alone takes %u bands", (unsigned)xsdk_gpu_tiling_bands(&tiling));
	if (!NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &s->commands)))
		return false;
	xsdk_gpu_set_shaders(&s->rect_vertex, &s->flat_pixel);
	xsdk_gpu_set_vertices(0, s->square, sizeof square);
	xsdk_gpu_set_depth_stencil(&s->less);
	for (uint32_t i = 0; i < BIG_RECTS; i++)
		draw_big_rect(&big_rects[i]);
	return NT_SUCCESS(xsdk_gpu_end_tiles(&resolve, 1)) && finished("2048x2048 depth");
}

// A texture over a target texel for texel, through the texture shaders.
static void draw_sampling(const Scene *s, const XsdkTexture *texture, uint32_t screen)
{
	const XsdkSampler point_clamped = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP };
	XsdkSamplerState sampler;
	xsdk_texture_sampler_state(&sampler, &point_clamped);
	xsdk_gpu_set_shaders(&s->textured_vertex, &s->textured_pixel);
	xsdk_gpu_set_vertices(0, screen, QUAD_VERTICES * TEXTURED_FLOATS * 4);
	xsdk_gpu_set_texture(TEXTURE_SLOT, texture, &sampler);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static bool draw_big_sampled(Scene *s)
{
	const XsdkTiling tiling = {
		.width = BIG, .height = BIG, .samples = 1, .targets = { { .format = XSDK_TARGET_8_8_8_8 } }, .count = 1
	};
	XsdkTileResolve resolve = { .target = 0, .texture = &s->big_sampled };
	rgba_of(0xFF000000, resolve.clear_rgba);
	if (!NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &s->commands)))
		return false;
	draw_sampling(s, &s->big_depth, s->big_screen);
	return NT_SUCCESS(xsdk_gpu_end_tiles(&resolve, 1)) && finished("2048x2048 sampled");
}

// The depth texture's top left 1280x720 on screen, untiled.
static void show_big(Scene *s)
{
	const XsdkTexture *const frame = &s->frames[s->frame++ % FRAMES];
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	draw_sampling(s, &s->big_depth, s->show_screen);
	xsdk_gpu_resolve_texture(frame, 0xFF000000);
	if (finished("2048x2048 shown"))
		show(s, frame);
}

static void check_big(Scene *s)
{
	const bool depth_drawn = draw_big_depth(s);
	const uint32_t wrong_depth = depth_drawn ? count_wrong_depth(&s->big_depth) : 0;
	xsdk_log_write(
		"tiles: 2048x2048 depth alone: %u of %u texels' depth wrong", (unsigned)wrong_depth, (unsigned)(BIG * BIG));
	check("tiles: 2048x2048 depth alone in 2 bands, resolved a band at a time, every texel's depth as drawn",
		depth_drawn && wrong_depth == 0);
	const bool sampled = depth_drawn && draw_big_sampled(s);
	const uint32_t wrong_samples = sampled ? count_wrong_samples(&s->big_sampled) : 0;
	xsdk_log_write("tiles: 2048x2048 sampled: %u of %u texels wrong", (unsigned)wrong_samples, (unsigned)(BIG * BIG));
	check("tiles: the 2048x2048 depth sampled by a quad in a tiled pass, every texel's red its depth",
		sampled && wrong_samples == 0);
	if (sampled)
		show_big(s);
}

// The last variant's frame, as its untiled reference, drawn back and resolved into itself by a pass of the variant's
// layout after untiled draws: band 0's clear mustn't resolve into it before band 0 draws it back.
static void check_drawn_back(Scene *s)
{
	const XsdkTexture *const frame = &s->frames[(s->frame + FRAMES - 1) % FRAMES];
	const XsdkTiling tiling = { .width = WIDTH,
		.height = HEIGHT,
		.samples = 1,
		.targets = { { .format = XSDK_TARGET_8_8_8_8 } },
		.count = 1,
		.depth = true,
		.band_rows = 256 };
	const XsdkTileResolve resolve = { .target = 0, .texture = frame, .drawn_back = true };
	bool drawn = NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &s->commands));
	if (drawn) {
		draw_sampling(s, frame, s->frame_screen);
		drawn = NT_SUCCESS(xsdk_gpu_end_tiles(&resolve, 1)) && finished("drawn back");
	}
	const uint32_t differing = drawn ? count_differing("drawn back", frame, 0, &s->reference, 0, HEIGHT, 4) : 0;
	xsdk_log_write("tiles: drawn back: %u of %u pixels differ", (unsigned)differing, (unsigned)(WIDTH * HEIGHT));
	check("tiles: a pass drawing its texture back first and resolving into it, every pixel as it was",
		drawn && differing == 0);
}

// Pixels of a 1280x720 B8G8R8A8 texture other than argb; a resolve stores B, G, R, A from the top byte.
static uint32_t count_other(const XsdkTexture *texture, uint32_t argb)
{
	const uint32_t want = (argb & 0xFF) << 24 | (argb >> 8 & 0xFF) << 16 | (argb >> 16 & 0xFF) << 8 | argb >> 24;
	uint32_t other = 0;
	for (uint32_t done = 0; done < HEIGHT; done += CHUNK_ROWS) {
		const uint32_t count = HEIGHT - done < CHUNK_ROWS ? HEIGHT - done : CHUNK_ROWS;
		xsdk_texture_download_rows(texture, 0, done, count, rows_a, WIDTH * 4);
		for (uint32_t i = 0; i < count * WIDTH; i++)
			other += first_word(&rows_a[i * 4]) != want;
	}
	return other;
}

// Two passes laid out the same back to back, drawing nothing: the second's band 0 holds its own clear, not the one the
// first's last band left.
static void check_back_to_back(Scene *s)
{
	const XsdkTiling tiling = { .width = WIDTH,
		.height = HEIGHT,
		.samples = 1,
		.targets = { { .format = XSDK_TARGET_8_8_8_8 } },
		.count = 1,
		.depth = true,
		.band_rows = 256 };
	bool drawn = true;
	for (uint32_t i = 0; i < 2 && drawn; i++) {
		XsdkTileResolve resolve = { .target = 0, .texture = &s->frames[i] };
		rgba_of(back_to_back_argb[i], resolve.clear_rgba);
		drawn = NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &s->commands)) && NT_SUCCESS(xsdk_gpu_end_tiles(&resolve, 1));
	}
	drawn = drawn && finished("back to back");
	const uint32_t other = drawn ? count_other(&s->frames[1], back_to_back_argb[1]) : 0;
	xsdk_log_write(
		"tiles: back to back: %u of %u pixels not the second's clear", (unsigned)other, (unsigned)(WIDTH * HEIGHT));
	check("tiles: two passes laid out the same back to back, the second's every pixel its own clear",
		drawn && other == 0);
	if (drawn)
		show(s, &s->frames[1]);
}

typedef struct {
	const char *what;
	XsdkTiling tiling;
	uint32_t bands;
} BandCase;

static const BandCase band_cases[] = {
	{ "1280x720, 8_8_8_8 and depth", { WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 0, false }, 1 },
	{ "the same in bands of 384 rows", { WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 384, false },
		2 },
	{ "the same in bands of 256 rows", { WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 256, false },
		3 },
	{ "the same with depth whole", { WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, true, 256, false }, 3 },
	{ "the same with depth whole at the top",
		{ WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, true, 256, true }, 3 },
	{ "depth at the top in bands", { WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 0, true }, 0 },
	{ "1280x720, four targets, one 64-bit, and depth",
		{ WIDTH, HEIGHT, 1,
			{ { .format = XSDK_TARGET_8_8_8_8 }, { .format = XSDK_TARGET_2_10_10_10 },
				{ .format = XSDK_TARGET_16_16_16_16_FLOAT }, { .format = XSDK_TARGET_32_FLOAT } },
			4, true, false, 0, false },
		3 },
	{ "2048x2048 depth alone", { BIG, BIG, 1, { { 0 } }, 0, true, false, 0, false }, 2 },
	{ "2048x2048 8_8_8_8 alone", { BIG, BIG, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, false, false, 0, false }, 2 },
	{ "1280x720, 8_8_8_8 and depth, at 2 samples",
		{ WIDTH, HEIGHT, 2, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 0, false }, 2 },
	{ "the same at 2 samples with depth whole",
		{ WIDTH, HEIGHT, 2, { { XSDK_TARGET_8_8_8_8 } }, 1, true, true, 0, false }, 3 },
	{ "1280x720, 8_8_8_8 and depth, at 4 samples",
		{ WIDTH, HEIGHT, 4, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 0, false }, 3 },
	{ "16_16_16_16_FLOAT at 2 samples, which doesn't take them",
		{ WIDTH, HEIGHT, 2, { { .format = XSDK_TARGET_16_16_16_16_FLOAT } }, 1, true, false, 0, false }, 0 },
	{ "3 samples", { WIDTH, HEIGHT, 3, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 0, false }, 0 },
	{ "bands of 48 rows", { WIDTH, HEIGHT, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 48, false }, 0 },
	{ "8193 rows", { WIDTH, 8193, 1, { { XSDK_TARGET_8_8_8_8 } }, 1, true, false, 0, false }, 0 },
	{ "nothing drawn", { WIDTH, HEIGHT, 1, { { 0 } }, 0, false, false, 0, false }, 0 },
	{ "8192x8192, four 64-bit targets and depth, past 64 bands",
		{ 8192, 8192, 1,
			{ { .format = XSDK_TARGET_16_16_16_16_FLOAT }, { .format = XSDK_TARGET_16_16_16_16_FLOAT },
				{ .format = XSDK_TARGET_16_16_16_16_FLOAT }, { .format = XSDK_TARGET_16_16_16_16_FLOAT } },
			4, true, false, 0, false },
		0 },
};

// Each pass's bands as its tiles say, and the passes that can't be tiled refused when begun.
static void check_band_counts(Scene *s)
{
	bool right = true;
	for (uint32_t i = 0; i < sizeof band_cases / sizeof band_cases[0]; i++) {
		const BandCase *const c = &band_cases[i];
		const uint32_t bands = xsdk_gpu_tiling_bands(&c->tiling);
		const NTSTATUS refused = STATUS_INVALID_PARAMETER;
		const NTSTATUS begun = bands ? refused : xsdk_gpu_begin_tiles(&c->tiling, &s->commands);
		xsdk_log_write("tiles: %s: %u bands, for %u%s", c->what, (unsigned)bands, (unsigned)c->bands,
			bands                  ? ""
				: begun == refused ? ", refused when begun"
								   : ", begun");
		right &= bands == c->bands && begun == refused;
	}
	check("tiles: band counts as the tiles say, and passes that can't be tiled refused", right);
}

static volatile uint32_t *upload(const void *data, uint32_t bytes)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(bytes);
	if (!memory)
		return NULL;
	for (uint32_t i = 0; i < bytes / 4; i++) {
		uint32_t word;
		memcpy(&word, (const uint8_t *)data + i * 4, sizeof word);
		memory[i] = word;
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), bytes);
	return memory;
}

// A quad over a width by height target, edges half a pixel early, its texture coordinates to u and v at the far edges.
static void screen_quad(float quad[QUAD_VERTICES][TEXTURED_FLOATS], uint32_t width, uint32_t height, float u, float v)
{
	const float left = -1.0f - 1.0f / width;
	const float right = 1.0f - 1.0f / width;
	const float top = 1.0f + 1.0f / height;
	const float bottom = -1.0f + 1.0f / height;
	const float corners[QUAD_VERTICES][4] = { { left, top, 0, 0 }, { right, top, u, 0 }, { left, bottom, 0, v },
		{ left, bottom, 0, v }, { right, top, u, 0 }, { right, bottom, u, v } };
	for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
		const float vertex[TEXTURED_FLOATS] = { corners[i][0], corners[i][1], 0.0f, 1.0f, corners[i][2],
			corners[i][3] };
		memcpy(quad[i], vertex, sizeof vertex);
	}
}

// The triangles' corners in clip space, exact as each is a whole pixel with x a multiple of 5 and y of 45.
static void make_triangles(float out[TRIANGLES * TRIANGLE_VERTICES][COLORED_FLOATS])
{
	for (uint32_t t = 0; t < TRIANGLES; t++) {
		for (uint32_t v = 0; v < TRIANGLE_VERTICES; v++) {
			const uint32_t argb = triangle_argb[t];
			const float vertex[COLORED_FLOATS] = { clip_x(triangle_corners[t][v][0], WIDTH),
				clip_y(triangle_corners[t][v][1], HEIGHT), 0.0f, 1.0f, unorm(argb >> 16), unorm(argb >> 8), unorm(argb),
				1.0f };
			memcpy(out[t * TRIANGLE_VERTICES + v], vertex, sizeof vertex);
		}
	}
}

static bool upload_vertices(Scene *s, volatile uint32_t *memory[VERTEX_BUFFERS])
{
	static float triangles[TRIANGLES * TRIANGLE_VERTICES][COLORED_FLOATS];
	static float big_screen[QUAD_VERTICES][TEXTURED_FLOATS];
	static float show_screen[QUAD_VERTICES][TEXTURED_FLOATS];
	static float frame_screen[QUAD_VERTICES][TEXTURED_FLOATS];
	make_triangles(triangles);
	screen_quad(big_screen, BIG, BIG, 1.0f, 1.0f);
	screen_quad(show_screen, WIDTH, HEIGHT, (float)WIDTH / BIG, (float)HEIGHT / BIG);
	screen_quad(frame_screen, WIDTH, HEIGHT, 1.0f, 1.0f);
	memory[0] = upload(square, sizeof square);
	memory[1] = upload(triangles, sizeof triangles);
	memory[2] = upload(big_screen, sizeof big_screen);
	memory[3] = upload(show_screen, sizeof show_screen);
	memory[4] = upload(frame_screen, sizeof frame_screen);
	for (uint32_t i = 0; i < VERTEX_BUFFERS; i++) {
		if (!memory[i])
			return false;
	}
	s->square = xsdk_gpu_address(memory[0]);
	s->triangles = xsdk_gpu_address(memory[1]);
	s->big_screen = xsdk_gpu_address(memory[2]);
	s->show_screen = xsdk_gpu_address(memory[3]);
	s->frame_screen = xsdk_gpu_address(memory[4]);
	return true;
}

// xost's compiler on the console, as the layer compiles what its bundle lacks (tests/test_xsc.c).
static bool compile_gradient(Scene *s)
{
	const XscTestShader *source = NULL;
	for (size_t i = 0; i < sizeof xsc_test_shaders / sizeof xsc_test_shaders[0]; i++) {
		if (!strcmp(xsc_test_shaders[i].name, "vk_frag_coord_rows.frag"))
			source = &xsc_test_shaders[i];
	}
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	return source && xsdk_xsc_compile(source->spirv, source->spirv_words, &options, &s->gradient) == XSDK_XSC_OK &&
		NT_SUCCESS(
			xsdk_shader_load(&s->gradient_pixel, (const uint32_t *)s->gradient.container, s->gradient.container_bytes));
}

static bool load_shaders(Scene *s)
{
	return NT_SUCCESS(xsdk_shader_load(&s->rect_vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&s->rect_vertex, square_layout, 1, (const uint32_t[]){ 16 })) &&
		NT_SUCCESS(xsdk_shader_load(&s->colored_vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&s->colored_vertex, colored_layout, 2, (const uint32_t[]){ COLORED_FLOATS * 4 })) &&
		NT_SUCCESS(xsdk_shader_load(&s->textured_vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&s->textured_vertex, textured_layout, 2, (const uint32_t[]){ TEXTURED_FLOATS * 4 })) &&
		NT_SUCCESS(xsdk_shader_load(&s->flat_pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&s->four_pixel, four_targets_pixel_shader, sizeof four_targets_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&s->textured_pixel, texture_pixel_shader, sizeof texture_pixel_shader)) &&
		compile_gradient(s);
}

static bool make_textures(Scene *s)
{
	bool made = NT_SUCCESS(xsdk_texture_create(&s->reference, XSDK_FORMAT_B8G8R8A8, WIDTH, HEIGHT, 1, true)) &&
		NT_SUCCESS(xsdk_texture_create(&s->half, XSDK_FORMAT_B8G8R8A8, WIDTH, HALF_ROWS, 1, true)) &&
		NT_SUCCESS(xsdk_texture_create(&s->big_depth, XSDK_FORMAT_D24S8, BIG, BIG, 1, true)) &&
		NT_SUCCESS(xsdk_texture_create(&s->big_sampled, XSDK_FORMAT_B8G8R8A8, BIG, BIG, 1, true)) &&
		NT_SUCCESS(xsdk_texture_create(&s->depth, XSDK_FORMAT_D24S8, WIDTH, HEIGHT, 1, true));
	for (uint32_t i = 0; made && i < FRAMES; i++)
		made = NT_SUCCESS(xsdk_texture_create(&s->frames[i], XSDK_FORMAT_B8G8R8A8, WIDTH, HEIGHT, 1, true));
	for (uint32_t i = 0; made && i < XSDK_COLOR_TARGETS; i++)
		made = NT_SUCCESS(xsdk_texture_create(&s->four[i], four_textures[i], WIDTH, HEIGHT, 1, true)) &&
			NT_SUCCESS(xsdk_texture_create(&s->pieces[i], four_textures[i], WIDTH, FOUR_ROWS, 1, true));
	return made;
}

static void free_textures(Scene *s)
{
	XsdkTexture *const all[] = { &s->reference, &s->half, &s->big_depth, &s->big_sampled, &s->depth, &s->frames[0],
		&s->frames[1], &s->four[0], &s->four[1], &s->four[2], &s->four[3], &s->pieces[0], &s->pieces[1], &s->pieces[2],
		&s->pieces[3] };
	for (uint32_t i = 0; i < sizeof all / sizeof all[0]; i++) {
		if (all[i]->texels)
			xsdk_texture_free(all[i]);
	}
}

static void free_shaders(Scene *s)
{
	XsdkShader *const all[] = { &s->rect_vertex, &s->colored_vertex, &s->textured_vertex, &s->flat_pixel,
		&s->four_pixel, &s->textured_pixel, &s->gradient_pixel };
	for (uint32_t i = 0; i < sizeof all / sizeof all[0]; i++)
		xsdk_shader_free(all[i]);
	xsdk_xsc_free(&s->gradient);
}

static void make_depth_states(Scene *s)
{
	const XsdkDepthStencil less = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	const XsdkDepthStencil off = { .depth_test = false };
	xsdk_gpu_depth_stencil_state(&s->less, &less);
	xsdk_gpu_depth_stencil_state(&s->off, &off);
}

static void run(Scene *s)
{
	check_band_counts(s);
	for (uint32_t i = 0; i < sizeof variants / sizeof variants[0]; i++)
		check_variant(s, &variants[i]);
	check_drawn_back(s);
	check_back_to_back(s);
	for (uint32_t i = 0; i < sizeof sampled_variants / sizeof sampled_variants[0]; i++)
		check_variant(s, &sampled_variants[i]);
	check_four(s);
	check_big(s);
	if (s->has_display)
		check("tiles: every frame shown", s->shown);
}

void test_tiles(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("tiles: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	static Scene scene;
	Scene *const s = &scene;
	memset(s, 0, sizeof *s);
	uint32_t width = 0;
	uint32_t height = 0;
	s->has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	s->shown = true;
	check("tiles: display open at 1280x720", s->has_display);
	make_depth_states(s);
	volatile uint32_t *memory[VERTEX_BUFFERS] = { NULL };
	const bool made = load_shaders(s) && upload_vertices(s, memory) && make_textures(s) &&
		NT_SUCCESS(xsdk_gpu_command_buffer_create(&s->commands, COMMAND_DWORDS));
	check("tiles: shaders, xost's gradient compiled, vertices, textures and the recording's buffer", made);
	if (made)
		run(s);
	xsdk_display_close();
	if (s->commands.dwords)
		xsdk_gpu_command_buffer_free(&s->commands);
	free_textures(s);
	for (uint32_t i = 0; i < VERTEX_BUFFERS; i++)
		xsdk_gpu_free(memory[i]);
	free_shaders(s);
	check("tiles: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
