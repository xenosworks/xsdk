#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "microcode_shaders.h"
#include "target_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

// A triangle drawn at 1, 2 and 4 samples, each resolved into a texture whose every pixel is checked: off the edges
// as the CPU works out, on them one of the blends a pixel's samples can make. Then the same through
// xsdk_gpu_set_targets_samples at 2 and 4 samples for each format it takes there, alone and four at once, each texel
// read back by value within the format's precision, with the step 3 probe's colours
// (results/2026-10-20-milestone-44-step-3/probe_msaa_default.log). Then alpha to coverage at 1, 2 and 4 samples, a band
// of squares at alphas 0 to 1 in 16ths for each count, shown on screen, every pixel the share of its samples the step 4
// probe found covered at its square's alpha (results/2026-10-20-milestone-44-step-4).

enum {
	// Resolved across its texture's 256-texel pitch, which at 4 samples takes more eDRAM tiles across than 240 does.
	TARGET_WIDTH = 240,
	TARGET_HEIGHT = 128,
	ROUNDS = 4,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	CLEAR_ARGB = 0xFF202020,
	// Each channel is the clear colour's plus a multiple of 4, so halves and quarters of the two are whole.
	SHAPE_ARGB = 0xFFE0A060,
	// A blend rounded either way still counts.
	TOLERANCE = 1,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	TRIANGLE_VERTICES = 3,
	MAX_SAMPLES = 4,
	// A pixel's samples sit within 3/8 of a pixel of its centre each way (docs/HARDWARE.md), well inside this.
	EDGE_DISTANCE = 1,
	LABEL_BYTES = 64,
	CHANNELS = 4,
	// constant_vertex_shader's position * c0 + c1 (tools/gen_constant_shaders.py).
	PLACE_CONSTANTS = 2,
	// Four half floats.
	MAX_TEXEL_BYTES = 8,
};

// Alpha to coverage: for each count a 1280x64 target, resolved into a band of a 1280x720 frame's rows, the bands one
// above another on screen.
enum {
	FRAME_WIDTH = 1280,
	FRAME_HEIGHT = 720,
	BAND_ROWS = 64,
	// From a multiple of 32 rows, where a resolve's destination moves in a tiled frame (docs/HARDWARE.md, "Tiling's
	// registers and packets, milestone 44 step 1").
	FIRST_BAND_TOP = 224,
	BAND_STEP = 96,
	// On even pixels, so each square is whole quads.
	SQUARE_LEFT = 72,
	SQUARE_TOP = 8,
	SQUARE_CELL = 64,
	SQUARE_SIZE = 48,
	ALPHA_STEPS = 16,
	// The squares at alphas 0 to 16/16, then one at 1/16 drawn after the target is set again.
	UNDONE_SQUARE = ALPHA_STEPS + 1,
	SQUARES = UNDONE_SQUARE + 1,
	ALPHA_CASES = 3,
	QUAD_VERTICES = 6,
	TEXT_BYTES = 192,
	// Red, green and blue; alpha is left out, as the squares' differ, as tests/probe_msaa.c reads them.
	RGB_LAST_SHIFT = 16,
	ALL_LAST_SHIFT = 24,
};

// A count's band: its colour, and the first alpha in 16ths at which each pixel covers 1, 2 and so on of its samples, as
// the probe found them (results/2026-10-20-milestone-44-step-4/probe_msaa_atoc_sym_<n>.log).
typedef struct {
	uint32_t samples;
	uint32_t shape_argb;
	uint8_t thresholds[MAX_SAMPLES];
} AlphaCase;

// Each channel the clear's plus a multiple of 4, as SHAPE_ARGB.
static const AlphaCase alpha_cases[ALPHA_CASES] = {
	{ 1, 0xFFE0A060, { 8 } },
	{ 2, 0xFF60E0A0, { 4, 12 } },
	{ 4, 0xFFA060E0, { 2, 6, 10, 14 } },
};

// How a texture holds a texel, as tests/test_targets.c reads them.
enum {
	// One byte a channel, red first.
	KIND_BYTES,
	// One word: red, green and blue in 10 bits from the low end, alpha in the top 2.
	KIND_10_10_10_2,
	// One half float a channel, red first.
	KIND_HALVES,
};

// A format xsdk_gpu_set_targets_samples takes at 2 and 4 samples, the texture it resolves into, how near a channel
// must be, this and this share of the value, and the probe's clear and colour, whose blends in quarters the format
// holds.
typedef struct {
	const char *name;
	uint32_t target;
	uint32_t texture;
	uint32_t kind;
	float absolute;
	float relative;
	float clear[CHANNELS];
	float color[CHANNELS];
} Format;

// The 64-bit one second, so four at once place a target after it.
static const Format formats[XSDK_COLOR_TARGETS] = {
	{ "8_8_8_8", XSDK_TARGET_8_8_8_8, XSDK_FORMAT_R8G8B8A8, KIND_BYTES, 1.5f / 255, 0.0f,
		{ 0.125f, 0.25f, 0.375f, 1.0f }, { 0.4375f, 0.0625f, 0.25f, 0.5f } },
	{ "16_16_16_16", XSDK_TARGET_16_16_16_16, XSDK_FORMAT_R16G16B16A16F, KIND_HALVES, 0.002f, 1.0f / 1024,
		{ 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 3.0f, -0.5f } },
	// Alpha in 2 bits holds 1/3 and stays out of the edges' blends.
	{ "2_10_10_10", XSDK_TARGET_2_10_10_10, XSDK_FORMAT_A2B10G10R10, KIND_10_10_10_2, 1.5f / 1023, 0.0f,
		{ 0.125f, 0.25f, 0.375f, 1.0f / 3 }, { 0.4375f, 0.0625f, 0.25f, 1.0f / 3 } },
	// 7e3's 7 bits of mantissa hold each step.
	{ "2_10_10_10_FLOAT", XSDK_TARGET_2_10_10_10_FLOAT, XSDK_FORMAT_R16G16B16A16F, KIND_HALVES, 0.002f, 1.0f / 128,
		{ 0.25f, 0.5f, 1.0f, 1.0f / 3 }, { 1.25f, 2.5f, 3.0f, 1.0f / 3 } },
};

// Where a pixel's centre lies.
enum {
	PIXEL_OUTSIDE,
	PIXEL_INSIDE,
	// Within EDGE_DISTANCE of an edge, where its samples may differ.
	PIXEL_EDGE,
};

typedef struct {
	float x;
	float y;
} Point;

typedef struct {
	uint32_t samples;
	// As vulkan4xenon places its targets, colour from tile 0 and depth at the top of eDRAM, else xsdk's layout.
	bool placed;
} Case;

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	uint32_t vertices;
	// Raster state set after the target, which must keep its samples.
	XsdkRasterState raster;
	// The triangle placed by constant_vertex_shader, coloured c0 by microcode_pixel_shader, or c0 to c3 into four
	// targets by four_targets_pixel_shader (tools/gen_microcode_shaders.py, gen_target_shaders.py).
	const XsdkShader *place_vertex;
	const XsdkShader *flat_pixel;
	const XsdkShader *four_pixel;
	// A unit square after the triangle, for constant_vertex_shader to place.
	uint32_t square;
} Scene;

typedef struct {
	// Edge pixels at k of the samples' blend, k samples the triangle's colour and the rest the clear colour.
	uint32_t levels[MAX_SAMPLES + 1];
	uint32_t unstepped;
	uint32_t wrong_off_edges;
} Tally;

// In pixels, each pixel the square from (x, y) to (x + 1, y + 1): a shallow edge, a steep one and a diagonal.
static const Point corners[TRIANGLE_VERTICES] = { { 16.3f, 12.7f }, { 224.1f, 40.4f }, { 40.6f, 116.2f } };

static const Case cases[] = {
	{ 1, false },
	{ 2, false },
	{ 4, false },
	{ 1, true },
	{ 2, true },
	{ 4, true },
};

enum { CASES = sizeof cases / sizeof cases[0] };

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static uint8_t places[TARGET_HEIGHT][TARGET_WIDTH];
static uint32_t readback[TARGET_WIDTH * TARGET_HEIGHT * MAX_TEXEL_BYTES / 4];

static float edge_side(Point a, Point b, float x, float y)
{
	return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

static void find_places(void)
{
	const float area = edge_side(corners[0], corners[1], corners[2].x, corners[2].y);
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			bool inside = true;
			bool edge = false;
			for (uint32_t i = 0; i < TRIANGLE_VERTICES; i++) {
				const Point a = corners[i];
				const Point b = corners[(i + 1) % TRIANGLE_VERTICES];
				const float side = edge_side(a, b, x + 0.5f, y + 0.5f);
				const float length = __builtin_sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
				inside = inside && side * area > 0.0f;
				edge = edge || __builtin_fabsf(side) <= EDGE_DISTANCE * length;
			}
			places[y][x] = edge ? PIXEL_EDGE : inside ? PIXEL_INSIDE : PIXEL_OUTSIDE;
		}
	}
}

// Edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837), in the triangle's colour; then the unit square.
static volatile uint32_t *upload_vertices(void)
{
	static const float unit_square[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
	float vertices[TRIANGLE_VERTICES + QUAD_VERTICES][VERTEX_FLOATS] = { { 0.0f } };
	const float colour[4] = { (SHAPE_ARGB >> 16 & 0xFF) / 255.0f, (SHAPE_ARGB >> 8 & 0xFF) / 255.0f,
		(SHAPE_ARGB & 0xFF) / 255.0f, (SHAPE_ARGB >> 24) / 255.0f };
	for (uint32_t i = 0; i < TRIANGLE_VERTICES; i++) {
		const float position[4] = { (corners[i].x - 0.5f) / (TARGET_WIDTH / 2.0f) - 1.0f,
			1.0f - (corners[i].y - 0.5f) / (TARGET_HEIGHT / 2.0f), 0.0f, 1.0f };
		memcpy(&vertices[i][0], position, sizeof position);
		memcpy(&vertices[i][4], colour, sizeof colour);
	}
	for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
		const float position[4] = { unit_square[i][0], unit_square[i][1], 0.0f, 1.0f };
		memcpy(&vertices[TRIANGLE_VERTICES + i][0], position, sizeof position);
	}
	volatile uint32_t *const memory = xsdk_gpu_alloc(sizeof vertices);
	if (!memory)
		return NULL;
	uint32_t words[sizeof vertices / 4];
	memcpy(words, vertices, sizeof vertices);
	for (uint32_t i = 0; i < sizeof vertices / 4; i++)
		memory[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), sizeof vertices);
	return memory;
}

static void set_target(const Case *c)
{
	if (!c->placed) {
		xsdk_gpu_set_target_samples(TARGET_WIDTH, TARGET_HEIGHT, c->samples);
		return;
	}
	const uint32_t tiles = xsdk_gpu_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, c->samples);
	xsdk_gpu_set_target_samples_at(TARGET_WIDTH, TARGET_HEIGHT, c->samples, 0, XSDK_EDRAM_TILES - tiles);
}

// A first resolve clears every sample to the clear colour, then the triangle draws and resolves into the texture.
static void draw_case(const Scene *scene, const Case *c, const XsdkTexture *texture)
{
	set_target(c);
	xsdk_gpu_resolve_texture(texture, CLEAR_ARGB);
	set_target(c);
	xsdk_gpu_set_raster(&scene->raster);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	xsdk_gpu_set_vertices(0, scene->vertices, TRIANGLE_VERTICES * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, TRIANGLE_VERTICES);
	xsdk_gpu_resolve_texture(texture, CLEAR_ARGB);
}

// Which k of samples the pixel is, k samples of the shape and the rest of the clear colour, in the channels from blue
// to last_shift's, or -1 for none.
static int blend_level(uint32_t argb, uint32_t shape_argb, uint32_t samples, uint32_t last_shift)
{
	for (uint32_t k = 0; k <= samples; k++) {
		bool matches = true;
		for (uint32_t shift = 0; shift <= last_shift; shift += 8) {
			const int clear = CLEAR_ARGB >> shift & 0xFF;
			const int shape = shape_argb >> shift & 0xFF;
			const int want = clear + (int)k * (shape - clear) / (int)samples;
			const int got = argb >> shift & 0xFF;
			matches = matches && got - want <= (int)TOLERANCE && want - got <= (int)TOLERANCE;
		}
		if (matches)
			return (int)k;
	}
	return -1;
}

// "4 samples, placed", and the like.
static const char *label(const Case *c, char text[LABEL_BYTES])
{
	snprintf(text, LABEL_BYTES, "%u sample%s%s", (unsigned)c->samples, c->samples > 1 ? "s" : "",
		c->placed ? ", placed" : "");
	return text;
}

static void log_miss(const Case *c, const char *what, uint32_t x, uint32_t y, uint32_t argb, uint32_t *logged)
{
	if (*logged >= MAX_MISSES_LOGGED)
		return;
	(*logged)++;
	char text[LABEL_BYTES];
	xsdk_log_write(
		"multisample: %s: %s (%u, %u) held %08X", label(c, text), what, (unsigned)x, (unsigned)y, (unsigned)argb);
}

// A resolve of an A8R8G8B8 texture leaves ARGB words (docs/HARDWARE.md, "Render to texture, milestone 8").
static void tally_case(const Case *c, const XsdkTexture *texture, Tally *tally, uint32_t *logged)
{
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			const uint32_t argb = texture->texels[tiled_index(x, y, texture->pitch)];
			if (places[y][x] != PIXEL_EDGE) {
				if (argb == (places[y][x] == PIXEL_INSIDE ? SHAPE_ARGB : CLEAR_ARGB))
					continue;
				tally->wrong_off_edges++;
				log_miss(c, "off the edges", x, y, argb, logged);
				continue;
			}
			const int level = blend_level(argb, SHAPE_ARGB, c->samples, ALL_LAST_SHIFT);
			if (level >= 0) {
				tally->levels[level]++;
				continue;
			}
			tally->unstepped++;
			log_miss(c, "on an edge", x, y, argb, logged);
		}
	}
}

// Each edge pixel a step, every step between the two colours seen, and every pixel off the edges as worked out.
static void report(const char *what, uint32_t samples, const Tally *tally)
{
	char levels[64];
	uint32_t length = 0;
	bool every_step = true;
	for (uint32_t k = 0; k <= samples; k++) {
		length += snprintf(levels + length, sizeof levels - length, "%s%u", k ? ", " : "", (unsigned)tally->levels[k]);
		every_step = every_step && (k == 0 || k == samples || tally->levels[k] > 0);
	}
	xsdk_log_write("multisample: %s: edge pixels by samples covered: %s; %u between steps, %u wrong off the edges",
		what, levels, (unsigned)tally->unstepped, (unsigned)tally->wrong_off_edges);
	// For 1, 2 and 4 samples.
	static const char *const steps[] = { "one colour or the other", "blend in halves", "blend in quarters" };
	char name[160];
	snprintf(name, sizeof name, "multisample: %s: edge pixels %s, the rest as worked out", what, steps[samples / 2]);
	check(name, every_step && tally->unstepped == 0 && tally->wrong_off_edges == 0);
}

static void check_case(const Case *c, const Tally *tally)
{
	char text[LABEL_BYTES];
	report(label(c, text), c->samples, tally);
}

static void draw_rounds(const Scene *scene, const XsdkTexture *textures)
{
	Tally tallies[CASES];
	memset(tallies, 0, sizeof tallies);
	uint32_t logged = 0;
	bool fences_landed = true;
	for (uint32_t round = 0; round < ROUNDS && fences_landed; round++) {
		for (uint32_t i = 0; i < CASES; i++)
			draw_case(scene, &cases[i], &textures[i]);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		for (uint32_t i = 0; i < CASES && fences_landed; i++)
			tally_case(&cases[i], &textures[i], &tallies[i], &logged);
	}
	check("multisample: every round's fence landed", fences_landed);
	for (uint32_t i = 0; i < CASES && fences_landed; i++)
		check_case(&cases[i], &tallies[i]);
}

static float half_value(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float mantissa = (float)(half & 0x3FF) / 1024.0f;
	const float magnitude = exponent ? ldexpf(1.0f + mantissa, (int)exponent - 15) : ldexpf(mantissa, -14);
	return half & 0x8000 ? -magnitude : magnitude;
}

static uint32_t texel_bytes(const Format *f)
{
	return f->kind == KIND_HALVES ? 2 * CHANNELS : 4;
}

// Channel c of texel (x, y) read back, as the texture's format holds it.
static float texel_channel(const Format *f, uint32_t x, uint32_t y, uint32_t c)
{
	const uint32_t index = y * TARGET_WIDTH + x;
	if (f->kind == KIND_BYTES)
		return ((const uint8_t *)readback)[index * CHANNELS + c] / 255.0f;
	if (f->kind == KIND_10_10_10_2) {
		const uint32_t word = readback[index];
		return c == 3 ? (float)(word >> 30) / 3.0f : (float)(word >> (10 * c) & 0x3FF) / 1023.0f;
	}
	uint16_t half;
	memcpy(&half, (const uint8_t *)readback + (index * CHANNELS + c) * sizeof half, sizeof half);
	return half_value(half);
}

// Whether the texel is k of samples' blend, k samples the colour and the rest the clear; written so a NaN is never
// near.
static bool texel_at(const Format *f, uint32_t x, uint32_t y, uint32_t k, uint32_t samples)
{
	for (uint32_t c = 0; c < CHANNELS; c++) {
		const float want = f->clear[c] + (float)k * (f->color[c] - f->clear[c]) / (float)samples;
		if (!(fabsf(texel_channel(f, x, y, c) - want) <= f->absolute + f->relative * fabsf(want)))
			return false;
	}
	return true;
}

static void log_texel(const char *what, const Format *f, const char *where, uint32_t x, uint32_t y, uint32_t *logged)
{
	if (*logged >= MAX_MISSES_LOGGED)
		return;
	(*logged)++;
	xsdk_log_write("multisample: %s: %s (%u, %u) held %g %g %g %g", what, where, (unsigned)x, (unsigned)y,
		texel_channel(f, x, y, 0), texel_channel(f, x, y, 1), texel_channel(f, x, y, 2), texel_channel(f, x, y, 3));
}

// The texture read back: off the edges the clear or the colour, on them a step of the blend.
static void tally_format(const char *what, const Format *f, uint32_t samples, Tally *tally)
{
	uint32_t logged = 0;
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			if (places[y][x] != PIXEL_EDGE) {
				if (texel_at(f, x, y, places[y][x] == PIXEL_INSIDE ? samples : 0, samples))
					continue;
				tally->wrong_off_edges++;
				log_texel(what, f, "off the edges", x, y, &logged);
				continue;
			}
			uint32_t k = 0;
			while (k <= samples && !texel_at(f, x, y, k, samples))
				k++;
			if (k <= samples) {
				tally->levels[k]++;
				continue;
			}
			tally->unstepped++;
			log_texel(what, f, "on an edge", x, y, &logged);
		}
	}
}

// The colours' targets from tile 0 one after another, as many tiles each as its format takes, depth at eDRAM's top.
static void set_format_targets(const XsdkColorTarget *targets, uint32_t count, uint32_t samples)
{
	const uint32_t depth_tile = XSDK_EDRAM_TILES - xsdk_gpu_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, samples);
	xsdk_gpu_set_targets_samples(TARGET_WIDTH, TARGET_HEIGHT, samples, targets, count, depth_tile);
}

// The first count formats' targets cleared by resolves, the triangle drawn into each in its colour at once, then each
// resolved into its texture.
static bool draw_formats(
	const Scene *scene, const Format *first, uint32_t count, uint32_t samples, const XsdkTexture *textures)
{
	static const float place[PLACE_CONSTANTS][4] = { { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 0.0f } };
	XsdkColorTarget targets[XSDK_COLOR_TARGETS];
	float colors[XSDK_COLOR_TARGETS][CHANNELS];
	uint32_t tile = 0;
	for (uint32_t i = 0; i < count; i++) {
		targets[i] = (XsdkColorTarget){ .format = first[i].target, .tile = tile };
		tile += xsdk_gpu_color_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, first[i].target, samples);
		memcpy(colors[i], first[i].color, sizeof colors[i]);
	}
	set_format_targets(targets, count, samples);
	for (uint32_t i = 0; i < count; i++)
		xsdk_gpu_resolve_target(i, &textures[i], first[i].clear);
	set_format_targets(targets, count, samples);
	xsdk_gpu_set_shaders(scene->place_vertex, count > 1 ? scene->four_pixel : scene->flat_pixel);
	xsdk_gpu_set_vertex_constants(0, &place[0][0], PLACE_CONSTANTS);
	xsdk_gpu_set_pixel_constants(0, &colors[0][0], count);
	xsdk_gpu_set_vertices(0, scene->vertices, TRIANGLE_VERTICES * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, TRIANGLE_VERTICES);
	for (uint32_t i = 0; i < count; i++)
		xsdk_gpu_resolve_target(i, &textures[i], first[i].clear);
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

// Each texture of the draw checked by value.
static void check_formats(
	const Scene *scene, const Format *first, uint32_t count, uint32_t samples, const XsdkTexture *textures)
{
	const bool drawn = draw_formats(scene, first, count, samples, textures);
	for (uint32_t i = 0; i < count; i++) {
		char what[LABEL_BYTES];
		if (count > 1)
			snprintf(what, sizeof what, "four at once at %u samples, target %u, %s", (unsigned)samples, (unsigned)i,
				first[i].name);
		else
			snprintf(what, sizeof what, "%s at %u samples", first[i].name, (unsigned)samples);
		Tally tally;
		memset(&tally, 0, sizeof tally);
		if (drawn) {
			xsdk_texture_download_rows(
				&textures[i], 0, 0, TARGET_HEIGHT, readback, TARGET_WIDTH * texel_bytes(&first[i]));
			tally_format(what, &first[i], samples, &tally);
		} else {
			tally.unstepped = 1;
			xsdk_log_write("multisample: %s: the fence didn't land", what);
		}
		report(what, samples, &tally);
	}
}

// 32-bit targets of 240x128 took 32, 64 and 128 tiles at 1, 2 and 4 samples on the console, and 64-bit ones twice as
// many (results/2026-10-20-milestone-44-step-3/probe_msaa_default.log, at 256x128, which takes as many).
static bool format_tiles_right(void)
{
	bool right = true;
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		for (uint32_t samples = 1; samples <= MAX_SAMPLES; samples *= 2) {
			const uint32_t want = 32 * samples << (formats[i].target == XSDK_TARGET_16_16_16_16);
			const uint32_t tiles =
				xsdk_gpu_color_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, formats[i].target, samples);
			if (tiles != want)
				xsdk_log_write("multisample: %s at %u samples takes %u tiles, not %u", formats[i].name,
					(unsigned)samples, (unsigned)tiles, (unsigned)want);
			right = right && tiles == want;
		}
	}
	return right;
}

// Each format alone at 2 and 4 samples, then the four at once.
static void check_targets_samples(const Scene *scene, const XsdkTexture *textures)
{
	for (uint32_t samples = 2; samples <= MAX_SAMPLES; samples *= 2) {
		for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
			check_formats(scene, &formats[i], 1, samples, &textures[i]);
		check_formats(scene, formats, XSDK_COLOR_TARGETS, samples, textures);
	}
}

// constant_vertex_shader's place for the unit square over pixels from square's cell of a band, edges half a pixel early
// (xenia:src/xenia/gpu/draw_util.cc:837).
static void place_square(uint32_t square)
{
	const float left = (SQUARE_LEFT + square * SQUARE_CELL - 0.5f) / (FRAME_WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (SQUARE_TOP - 0.5f) / (BAND_ROWS / 2.0f);
	const float place[PLACE_CONSTANTS][4] = {
		{ SQUARE_SIZE / (FRAME_WIDTH / 2.0f), -SQUARE_SIZE / (BAND_ROWS / 2.0f), 1.0f, 1.0f },
		{ left, top, 0.0f, 0.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &place[0][0], PLACE_CONSTANTS);
}

// A count's band: eDRAM cleared by a resolve into the band's rows, the squares drawn under alpha to coverage, the last
// after the target is set again, then resolved into the rows. 4 samples sets it through xsdk_gpu_set_blends, so both
// calls are checked.
static void draw_alpha_band(const Scene *scene, const AlphaCase *a, const XsdkTexture *frame, uint32_t top)
{
	static const XsdkBlend coverage = { .color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.write_mask = XSDK_WRITE_ALL,
		.alpha_to_coverage = true };
	XsdkBlendState state;
	xsdk_gpu_blend_state(&state, &coverage);
	// A tiled frame's row top, a multiple of 32, starts top rows of its pitch in.
	const uint32_t address = frame->address + top * frame->pitch * 4;
	xsdk_gpu_set_target_samples(FRAME_WIDTH, BAND_ROWS, a->samples);
	xsdk_gpu_resolve(address, FRAME_WIDTH, BAND_ROWS, CLEAR_ARGB);
	xsdk_gpu_set_target_samples(FRAME_WIDTH, BAND_ROWS, a->samples);
	xsdk_gpu_set_shaders(scene->place_vertex, scene->flat_pixel);
	xsdk_gpu_set_vertices(0, scene->square, QUAD_VERTICES * STRIDE_BYTES);
	if (a->samples == MAX_SAMPLES)
		xsdk_gpu_set_blends(&state, 1);
	else
		xsdk_gpu_set_blend(&state);
	for (uint32_t square = 0; square < SQUARES; square++) {
		if (square == UNDONE_SQUARE)
			xsdk_gpu_set_target_samples(FRAME_WIDTH, BAND_ROWS, a->samples);
		const uint32_t argb = a->shape_argb;
		const float alpha = square == UNDONE_SQUARE ? 1.0f / ALPHA_STEPS : (float)square / ALPHA_STEPS;
		const float color[CHANNELS] = { (argb >> 16 & 0xFF) / 255.0f, (argb >> 8 & 0xFF) / 255.0f,
			(argb & 0xFF) / 255.0f, alpha };
		place_square(square);
		xsdk_gpu_set_pixel_constants(0, color, 1);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
	xsdk_gpu_resolve(address, FRAME_WIDTH, BAND_ROWS, CLEAR_ARGB);
}

// The samples a pixel of a band should cover, and its square or SQUARES off them: the probe's at the square's alpha,
// every one in the square drawn after the target is set again, none off the squares.
static uint32_t covered_at(const AlphaCase *a, uint32_t x, uint32_t y, uint32_t *square)
{
	const uint32_t in_x = x - SQUARE_LEFT;
	*square = in_x / SQUARE_CELL;
	if (x < SQUARE_LEFT || *square >= SQUARES || in_x % SQUARE_CELL >= SQUARE_SIZE || y < SQUARE_TOP ||
		y >= SQUARE_TOP + SQUARE_SIZE) {
		*square = SQUARES;
		return 0;
	}
	if (*square == UNDONE_SQUARE)
		return a->samples;
	uint32_t covered = 0;
	while (covered < a->samples && a->thresholds[covered] <= *square)
		covered++;
	return covered;
}

typedef struct {
	// Pixels of the alpha squares, and off the squares, unlike the probe's; and of the square after the target was set.
	uint32_t wrong;
	uint32_t undone_wrong;
	// Each square's first pixel's samples covered, or -1 for a colour that's no step, to log.
	int levels[SQUARES];
} AlphaTally;

// Every pixel of the band, as ARGB once its word's bytes are swapped (docs/HARDWARE.md, "Render to texture, milestone
// 8").
static void tally_alpha_band(const AlphaCase *a, const XsdkTexture *frame, uint32_t top, AlphaTally *t)
{
	uint32_t logged = 0;
	for (uint32_t y = 0; y < BAND_ROWS; y++) {
		for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
			const uint32_t argb = __builtin_bswap32(frame->texels[tiled_index(x, top + y, frame->pitch)]);
			uint32_t square;
			const uint32_t want = covered_at(a, x, y, &square);
			const int level = blend_level(argb, a->shape_argb, a->samples, RGB_LAST_SHIFT);
			if (square < SQUARES && x == SQUARE_LEFT + square * SQUARE_CELL && y == SQUARE_TOP)
				t->levels[square] = level;
			if (level == (int)want)
				continue;
			if (square == UNDONE_SQUARE)
				t->undone_wrong++;
			else
				t->wrong++;
			if (logged++ < MAX_MISSES_LOGGED)
				xsdk_log_write("multisample: alpha to coverage at %u samples: (%u, %u) held %08X, not %u covered",
					(unsigned)a->samples, (unsigned)x, (unsigned)(top + y), (unsigned)argb, (unsigned)want);
		}
	}
}

static void report_alpha_band(const AlphaCase *a, const AlphaTally *t, bool landed)
{
	char levels[TEXT_BYTES];
	uint32_t length = 0;
	for (uint32_t i = 0; i < ALPHA_STEPS + 1; i++)
		length += (uint32_t)snprintf(levels + length, sizeof levels - length, " %d", t->levels[i]);
	if (landed)
		xsdk_log_write(
			"multisample: alpha to coverage at %u samples: samples covered at alphas 0 to 16/16:%s; at "
			"1/16 after the target set again: %d",
			(unsigned)a->samples, levels, t->levels[UNDONE_SQUARE]);
	char name[TEXT_BYTES];
	snprintf(name, sizeof name,
		"multisample: alpha to coverage at %u sample%s%s: each square covers the samples the probe found at its "
		"alpha, every pixel, the rest the clear",
		(unsigned)a->samples, a->samples > 1 ? "s" : "",
		a->samples == MAX_SAMPLES ? ", through xsdk_gpu_set_blends" : "");
	check(name, landed && t->wrong == 0);
}

// The frame cleared by two resolves, the first clearing eDRAM, each count's band drawn, every band pixel checked, then
// the frame shown.
static void check_alpha_to_coverage(const Scene *scene, const XsdkTexture *frame)
{
	uint32_t width = 0;
	uint32_t height = 0;
	const bool display = NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == FRAME_WIDTH &&
		height == FRAME_HEIGHT;
	for (uint32_t i = 0; i < 2; i++) {
		xsdk_gpu_set_target(FRAME_WIDTH, FRAME_HEIGHT);
		xsdk_gpu_resolve(frame->address, FRAME_WIDTH, FRAME_HEIGHT, CLEAR_ARGB);
	}
	for (uint32_t i = 0; i < ALPHA_CASES; i++)
		draw_alpha_band(scene, &alpha_cases[i], frame, FIRST_BAND_TOP + i * BAND_STEP);
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	if (!landed)
		xsdk_log_write("multisample: alpha to coverage: the fence didn't land");
	uint32_t undone_wrong = 0;
	for (uint32_t i = 0; i < ALPHA_CASES; i++) {
		AlphaTally tally = { .wrong = 0 };
		if (landed)
			tally_alpha_band(&alpha_cases[i], frame, FIRST_BAND_TOP + i * BAND_STEP, &tally);
		report_alpha_band(&alpha_cases[i], &tally, landed);
		undone_wrong += tally.undone_wrong;
	}
	check(
		"multisample: alpha to coverage: off again once the target is set, alpha 1/16 covering every sample at 1, 2 "
		"and 4 samples",
		landed && undone_wrong == 0);
	const bool shown = display && NT_SUCCESS(xsdk_display_show(frame->address, frame->pitch));
	if (shown)
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	xsdk_display_close();
	check("multisample: alpha to coverage: the squares shown at 1280x720", shown);
}

void test_multisample(void)
{
	// 80x16 samples a tile, 4 samples two across (xenia:src/xenia/gpu/xenos.h:414-437), over whole 32-pixel columns.
	check("multisample: a 240x128 target takes 32, 64 and 128 eDRAM tiles at 1, 2 and 4 samples",
		xsdk_gpu_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, 1) == 32 &&
			xsdk_gpu_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, 2) == 64 &&
			xsdk_gpu_target_sample_tiles(TARGET_WIDTH, TARGET_HEIGHT, 4) == 128 &&
			xsdk_gpu_target_tiles(TARGET_WIDTH, TARGET_HEIGHT) == 32);
	check(
		"multisample: 8_8_8_8, 16_16_16_16, 2_10_10_10 and its 7e3 float take their tiles at 1, 2 and 4 samples, a "
		"64-bit target twice a 32-bit one's",
		format_tiles_right());
	const NTSTATUS status = xsdk_gpu_open();
	check("multisample: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	XsdkShader place_vertex = { .microcode = NULL };
	XsdkShader flat_pixel = { .microcode = NULL };
	XsdkShader four_pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_load(&place_vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&place_vertex, layout, 1, (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_load(&flat_pixel, microcode_pixel_shader, sizeof microcode_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&four_pixel, four_targets_pixel_shader, sizeof four_targets_pixel_shader));
	check("multisample: shaders load and patch", loaded);
	volatile uint32_t *const vertices = upload_vertices();
	XsdkTexture textures[CASES];
	XsdkTexture format_textures[XSDK_COLOR_TARGETS];
	bool made = vertices != NULL;
	for (uint32_t i = 0; i < CASES; i++) {
		textures[i] = (XsdkTexture){ .texels = NULL };
		const NTSTATUS created =
			xsdk_texture_create(&textures[i], XSDK_FORMAT_A8R8G8B8, TARGET_WIDTH, TARGET_HEIGHT, 1, true);
		made = made && NT_SUCCESS(created);
	}
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		format_textures[i] = (XsdkTexture){ .texels = NULL };
		const NTSTATUS created =
			xsdk_texture_create(&format_textures[i], formats[i].texture, TARGET_WIDTH, TARGET_HEIGHT, 1, true);
		made = made && NT_SUCCESS(created);
	}
	XsdkTexture frame = { .texels = NULL };
	made = made && NT_SUCCESS(xsdk_texture_create(&frame, XSDK_FORMAT_B8G8R8A8, FRAME_WIDTH, FRAME_HEIGHT, 1, true));
	check("multisample: vertices and textures", made);
	find_places();
	if (loaded && made) {
		Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.vertices = xsdk_gpu_address(vertices),
			.place_vertex = &place_vertex,
			.flat_pixel = &flat_pixel,
			.four_pixel = &four_pixel,
			.square = xsdk_gpu_address(vertices) + TRIANGLE_VERTICES * STRIDE_BYTES };
		const XsdkRaster raster = { .cull = XSDK_CULL_NONE, .front_clockwise = true };
		xsdk_gpu_raster_state(&scene.raster, &raster);
		draw_rounds(&scene, textures);
		check_targets_samples(&scene, format_textures);
		check_alpha_to_coverage(&scene, &frame);
	}
	for (uint32_t i = 0; i < CASES; i++)
		xsdk_texture_free(&textures[i]);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_texture_free(&format_textures[i]);
	xsdk_texture_free(&frame);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&four_pixel);
	xsdk_shader_free(&flat_pixel);
	xsdk_shader_free(&place_vertex);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("multisample: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
