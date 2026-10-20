#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "hiz_shaders.h"
#include "microcode_shaders.h"
#include "target_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Milestone 44 step 3's probe (docs/scope/44-tiling-and-msaa.md, "First, probe_msaa extended") of what Xenos does at 2
// and 4 samples that xsdk hasn't tried, and step 4's of alpha to coverage ("First, a probe"). game:\msaa_case.txt names
// a case, run with the cases after it in its group, or a group; without it the cases are listed and the default group
// runs. A fresh boot case writes register values xsdk never has, so it runs alone after a reboot, as a hang leaves the
// GPU hung. Each step is logged before it's sent and ends with a fence and a resolve the CPU reads back. Built with
// src/gpu_draw.c's probe hooks (Makefile, msaa_probes).

// The hooks XSDK_MSAA_PROBE builds into src/gpu_draw.c: colour targets of any format at any count, and the resolves'
// copy sample select (UINT32_MAX for xsdk's). xsdk keeps the hierarchical buffer on at 1280x256 at 2 and 4 samples
// since step 3, as this probe found it right there.
void xsdk_probe_msaa_targets(uint32_t width, uint32_t height, const XsdkColorTarget *targets, uint32_t count,
	uint32_t depth_tile, uint32_t samples);
void xsdk_probe_msaa_select(uint32_t select);

enum {
	// The format, four-target, pattern and depth cases' target, milestone 32's.
	WIDTH = 256,
	HEIGHT = 128,
	// The hierarchical depth and rate cases': as tall as fits eDRAM with depth at 4 samples, 1024 tiles each
	// (xenia:src/xenia/gpu/xenos.h:414-437).
	WIDE_WIDTH = 1280,
	WIDE_HEIGHT = 256,
	FRAME_HEIGHT = 720,
	FENCE_TIMEOUT_MS = 2000,
	MAX_SAMPLES = 4,
	CHANNELS = 4,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_VERTICES = 6,
	TRIANGLE_VERTICES = 3,
	SQUARE_FIRST = 0,
	TRIANGLE_FIRST = SQUARE_FIRST + QUAD_VERTICES,
	ACROSS_FIRST = TRIANGLE_FIRST + TRIANGLE_VERTICES,
	DOWN_FIRST = ACROSS_FIRST + QUAD_VERTICES,
	VERTICES = DOWN_FIRST + QUAD_VERTICES,
	// constant_vertex_shader's position * c0 + c1 and colour c2 (tools/gen_constant_shaders.py).
	VERTEX_CONSTANTS = 3,
	// Right of the triangle: the top quad drawn once, the bottom one drawn and then blended onto itself.
	REGION_LEFT = 192,
	REGION_SPLIT = 64,
	// Samples sit within 3/8 of a pixel of its centre (docs/HARDWARE.md, "Multisampled targets, milestone 32").
	EDGE_DISTANCE = 1,
	// The sweeps' row and column, and 16ths of a pixel, the sample offsets' unit.
	SWEEP_ROW = 64,
	SWEEP_COLUMN = 128,
	SIXTEENTHS = 16,
	MAX_LOGGED = 4,
	TEXT_BYTES = 256,
	LABEL_BYTES = 96,
	NAME_BYTES = 16,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc).
	RB_HIZCONTROL = 0x2203,
	PA_SC_AA_MASK = 0x2312,
	AA_MASK_ALL = 0xFFFF,
	// No public source gives PA_SC_AA_MASK's fields, so a sample's bit goes in every nibble.
	AA_MASK_NIBBLES = 0x1111,
	// RB_COPY_CONTROL's copy_sample_select (xenia:src/xenia/gpu/xenos.h:1000-1008).
	SELECT_01 = 4,
	SELECT_23 = 5,
	SELECT_0123 = 6,
	// The depth the depth case draws, as 24 bits, and how near a word must be.
	DEPTH_24_MAX = 0xFFFFFF,
	DEPTH_24_TOLERANCE = 2,
	// The GPU's clock counts 498.75 a microsecond (docs/HARDWARE.md, "Queries and events").
	CLOCKS_IN_4_US = 1995,
	CLOCK_BYTES = XSDK_GPU_CLOCK_WORDS * 4,
};

// Apart, as a value past INT_MAX makes an enum's constants unsigned (docs/HARDWARE.md, "Raster state, vulkan4xenon
// milestone 26").
enum {
	CLEAR_ARGB = 0xFF202020,
	// Each channel the clear's plus a multiple of 4, so halves and quarters of the two are whole.
	SHAPE_ARGB = 0xFFE0A060,
	// What a neighbouring target holds until something overwrites it.
	SENTINEL_ARGB = 0xFF00FF40,
	NEAR_ARGB = 0xFF40E040,
	FAR_ARGB = 0xFFE04040,
	PROBE_ARGB = 0xFFE0E040,
	// xsdk's RB_COLORCONTROL, libxenon's: alpha to mask off, its offsets 3, 1, 0, 2 (src/gpu_draw.c).
	COLOR_CONTROL = 0x87000007,
};

static const float DEPTH_DRAWN = 0.25f;

typedef struct {
	float x;
	float y;
} Point;

// In pixels, each pixel the square from (x, y) to (x + 1, y + 1): a triangle with a shallow, a steep and a diagonal
// edge left of the quads; and milestone 32's sweeps, an edge rising a pixel over the width through row 64, the shape
// below it, and one moving a pixel right over the height through column 128, the shape right of it.
static const Point triangle[TRIANGLE_VERTICES] = { { 12.3f, 10.7f }, { 170.1f, 30.4f }, { 30.6f, 118.2f } };
static const Point across[QUAD_VERTICES] = { { 0, 64 }, { 256, 65 }, { 0, 128 }, { 0, 128 }, { 256, 65 },
	{ 256, 128 } };
static const Point down[QUAD_VERTICES] = { { 128, 0 }, { 256, 0 }, { 129, 128 }, { 129, 128 }, { 256, 0 },
	{ 256, 128 } };
static const float unit_square[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

// How a texture holds a texel, as tests/test_targets.c reads them.
enum {
	// One byte a channel, red first.
	KIND_BYTES,
	// One word: red, green and blue in 10 bits from the low end, alpha in the top 2.
	KIND_10_10_10_2,
	// One half float a channel, red first.
	KIND_HALVES,
	// One float, red.
	KIND_FLOATS,
};

// A target format, the texture xsdk resolves it into, the clear and the colour drawn, past 1 where it holds that and
// at most 1/2 where it's unorm, so doubled it stays inside.
typedef struct {
	const char *name;
	uint32_t target;
	uint32_t texture;
	uint32_t kind;
	uint32_t channels;
	// How near a channel must be: this, and this share of the value for the floats.
	float absolute;
	float relative;
	float clear[CHANNELS];
	float color[CHANNELS];
	// What a quad blended onto itself did at 1 sample (docs/HARDWARE.md, "Float, 10-bit and 64-bit targets and four at
	// once").
	bool blends;
} Format;

enum {
	FORMAT_8_8_8_8,
	FORMAT_2_10_10_10,
	FORMAT_2_10_10_10_FLOAT,
	FORMAT_16_16_16_16,
	FORMAT_16_16_FLOAT,
	FORMAT_16_16_16_16_FLOAT,
	FORMAT_32_FLOAT,
	FORMATS,
};

static const Format formats[FORMATS] = {
	[FORMAT_8_8_8_8] = { "8_8_8_8", XSDK_TARGET_8_8_8_8, XSDK_FORMAT_R8G8B8A8, KIND_BYTES, 4, 1.5f / 255, 0.0f,
		{ 0.125f, 0.25f, 0.375f, 1.0f }, { 0.4375f, 0.0625f, 0.25f, 0.5f }, true },
	// Alpha in 2 bits holds 1/3 and doubled 2/3, and stays out of the edges' blends.
	[FORMAT_2_10_10_10] = { "2_10_10_10", XSDK_TARGET_2_10_10_10, XSDK_FORMAT_A2B10G10R10, KIND_10_10_10_2, 4,
		1.5f / 1023, 0.0f, { 0.125f, 0.25f, 0.375f, 1.0f / 3 }, { 0.4375f, 0.0625f, 0.25f, 1.0f / 3 }, true },
	// 7e3's 7 bits of mantissa hold each step of the edges' blends.
	[FORMAT_2_10_10_10_FLOAT] = { "2_10_10_10_FLOAT", XSDK_TARGET_2_10_10_10_FLOAT, XSDK_FORMAT_R16G16B16A16F,
		KIND_HALVES, 4, 0.002f, 1.0f / 128, { 0.25f, 0.5f, 1.0f, 1.0f / 3 }, { 1.25f, 2.5f, 3.0f, 1.0f / 3 }, true },
	[FORMAT_16_16_16_16] = { "16_16_16_16", XSDK_TARGET_16_16_16_16, XSDK_FORMAT_R16G16B16A16F, KIND_HALVES, 4, 0.002f,
		1.0f / 1024, { 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 3.0f, -0.5f }, true },
	[FORMAT_16_16_FLOAT] = { "16_16_FLOAT", XSDK_TARGET_16_16_FLOAT, XSDK_FORMAT_R16G16F, KIND_HALVES, 2, 0.0f,
		1.0f / 1024, { 0.25f, 0.5f }, { 1.5f, 1000.0f }, false },
	[FORMAT_16_16_16_16_FLOAT] = { "16_16_16_16_FLOAT", XSDK_TARGET_16_16_16_16_FLOAT, XSDK_FORMAT_R16G16B16A16F,
		KIND_HALVES, 4, 0.0f, 1.0f / 1024, { 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 1000.0f, -2.0f }, false },
	[FORMAT_32_FLOAT] = { "32_FLOAT", XSDK_TARGET_32_FLOAT, XSDK_FORMAT_R32F, KIND_FLOATS, 1, 0.0f, 1.0f / 65536,
		{ 0.25f }, { 1234.5f }, false },
};

// Milestone 25's four at once (tests/test_targets.c).
static const uint32_t four_formats[XSDK_COLOR_TARGETS] = { FORMAT_8_8_8_8, FORMAT_2_10_10_10, FORMAT_16_16_16_16_FLOAT,
	FORMAT_32_FLOAT };

// Where a pixel of the 256x128 scene lies.
enum {
	PLACE_OUTSIDE,
	PLACE_INSIDE,
	// Within EDGE_DISTANCE of a triangle edge, where its samples may differ.
	PLACE_EDGE,
	PLACE_QUAD,
	PLACE_BLEND,
};

static const char *const start_path = "game:\\msaa_case.txt";

static XsdkShader vertex_shader;
// microcode_pixel_shader writes c0, four_targets_pixel_shader c0 to c3 to oC0 to oC3, triangle_pixel_shader the
// vertex shader's colour, and long_pixel_shader that colour after 64 multiply-adds by c0.
static XsdkShader flat_shader;
static XsdkShader four_shader;
static XsdkShader colour_shader;
static XsdkShader long_shader;
static uint32_t vertices_address;
static volatile uint32_t *image;
static uint32_t scratch_address;
static volatile uint32_t *clocks;
static XsdkBlendState opaque_blend;
static XsdkBlendState additive_blend;
static XsdkBlendState alpha_blend;
static XsdkDepthStencilState less_state;
static XsdkDepthStencilState always_write_state;
static uint8_t places[HEIGHT][WIDTH];
static uint32_t readback[WIDTH * HEIGHT * 2];
static uint32_t passed;
static uint32_t differed;
static bool stopped;

static const float shape_color[CHANNELS] = { 224 / 255.0f, 160 / 255.0f, 96 / 255.0f, 1.0f };

static void verdict(bool held, const char *text)
{
	xsdk_log_write("%s: msaa: %s", held ? "PASS" : "DIFFERS", text);
	if (held)
		passed++;
	else
		differed++;
}

// False, logged, once a fence doesn't land: the GPU may be wedged, so nothing more is sent.
static bool wait_gpu(const char *step)
{
	if (stopped)
		return false;
	if (NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
		return true;
	xsdk_log_write("msaa: FAILED, the GPU didn't finish %s; stopping", step);
	stopped = true;
	return false;
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static float unorm_8(uint32_t argb, uint32_t shift)
{
	return (argb >> shift & 0xFF) / 255.0f;
}

// constant_vertex_shader's constants: the position times scale plus offset, at depth z, coloured argb.
static void place(float scale_x, float scale_y, float offset_x, float offset_y, float z, uint32_t argb)
{
	const float constants[VERTEX_CONSTANTS][4] = {
		{ scale_x, scale_y, 1.0f, 1.0f },
		{ offset_x, offset_y, z, 0.0f },
		{ unorm_8(argb, 16), unorm_8(argb, 8), unorm_8(argb, 0), unorm_8(argb, 24) },
	};
	xsdk_gpu_set_vertex_constants(0, &constants[0][0], VERTEX_CONSTANTS);
}

// The unit square over pixels x0 to x1 - 1 and y0 to y1 - 1 of a width by height target, edges half a pixel early
// (xenia:src/xenia/gpu/draw_util.cc:837).
static void place_rect(
	uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t width, uint32_t height, float z, uint32_t argb)
{
	const float left = (x0 - 0.5f) / (width / 2.0f) - 1.0f;
	const float right = (x1 - 0.5f) / (width / 2.0f) - 1.0f;
	const float top = 1.0f - (y0 - 0.5f) / (height / 2.0f);
	const float bottom = 1.0f - (y1 - 0.5f) / (height / 2.0f);
	place(right - left, bottom - top, left, top, z, argb);
	xsdk_gpu_set_vertices(0, vertices_address + SQUARE_FIRST * STRIDE_BYTES, QUAD_VERTICES * STRIDE_BYTES);
}

static void draw_quad(void)
{
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static void draw_rect(
	uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t width, uint32_t height, float z, uint32_t argb)
{
	place_rect(x0, y0, x1, y1, width, height, z, argb);
	draw_quad();
}

// A shape uploaded in the 256x128 target's clip space, at depth z.
static void draw_shape(uint32_t first, uint32_t count, float z)
{
	place(1.0f, 1.0f, 0.0f, 0.0f, z, SHAPE_ARGB);
	xsdk_gpu_set_vertices(0, vertices_address + first * STRIDE_BYTES, count * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, count);
}

// A resolved 8_8_8_8 pixel as ARGB (docs/HARDWARE.md, "Render to texture, milestone 8").
static uint32_t pixel_argb(uint32_t x, uint32_t y, uint32_t width)
{
	return __builtin_bswap32(image[tiled_index(x, y, width)]);
}

static bool near_argb(uint32_t a, uint32_t b, uint32_t tolerance)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t channel_a = a >> shift & 0xFF;
		const uint32_t channel_b = b >> shift & 0xFF;
		if ((channel_a > channel_b ? channel_a - channel_b : channel_b - channel_a) > tolerance)
			return false;
	}
	return true;
}

// The tiles xsdk counts for a 256x128 target of the format, a 64-bit one twice a 32-bit one's at any count
// (xenia:src/xenia/gpu/xenos.h:426-437).
static uint32_t format_tiles(const Format *f, uint32_t samples)
{
	return xsdk_gpu_color_target_sample_tiles(WIDTH, HEIGHT, f->target, samples);
}

// Depth at eDRAM's top, clear of the colour targets from tile 0.
static uint32_t depth_tile_for(uint32_t samples)
{
	return XSDK_EDRAM_TILES - xsdk_gpu_target_sample_tiles(WIDTH, HEIGHT, samples);
}

static float half_value(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float mantissa = (float)(half & 0x3FF) / 1024.0f;
	const float magnitude = exponent ? ldexpf(1.0f + mantissa, (int)exponent - 15) : ldexpf(mantissa, -14);
	return half & 0x8000 ? -magnitude : magnitude;
}

// Channel c of texel (x, y) read back, as the texture's format holds it.
static float texel_channel(const Format *f, uint32_t x, uint32_t y, uint32_t c)
{
	const uint32_t index = y * WIDTH + x;
	if (f->kind == KIND_BYTES)
		return ((const uint8_t *)readback)[index * f->channels + c] / 255.0f;
	if (f->kind == KIND_10_10_10_2) {
		const uint32_t word = readback[index];
		return c == 3 ? (float)(word >> 30) / 3.0f : (float)(word >> (10 * c) & 0x3FF) / 1023.0f;
	}
	if (f->kind == KIND_HALVES) {
		uint16_t half;
		memcpy(&half, (const uint8_t *)readback + (index * f->channels + c) * sizeof half, sizeof half);
		return half_value(half);
	}
	float value;
	memcpy(&value, &readback[index * f->channels + c], sizeof value);
	return value;
}

static uint32_t texel_bytes(const Format *f)
{
	switch (f->kind) {
	case KIND_BYTES:
		return f->channels;
	case KIND_HALVES:
		return 2 * f->channels;
	default:
		return 4;
	}
}

// Written so a NaN, which a float target can hold, is never near.
static bool texel_near(const Format *f, uint32_t x, uint32_t y, const float want[CHANNELS])
{
	for (uint32_t c = 0; c < f->channels; c++) {
		const float tolerance = f->absolute + f->relative * fabsf(want[c]);
		if (!(fabsf(texel_channel(f, x, y, c) - want[c]) <= tolerance))
			return false;
	}
	return true;
}

// k of samples' blend: k samples of the colour, the rest of the clear.
static void level_color(const Format *f, uint32_t k, uint32_t samples, float out[CHANNELS])
{
	for (uint32_t c = 0; c < CHANNELS; c++)
		out[c] = f->clear[c] + (float)k * (f->color[c] - f->clear[c]) / (float)samples;
}

// Which k of samples' blend the texel holds, or -1 for none.
static int texel_level(const Format *f, uint32_t x, uint32_t y, uint32_t samples)
{
	for (uint32_t k = 0; k <= samples; k++) {
		float want[CHANNELS];
		level_color(f, k, samples, want);
		if (texel_near(f, x, y, want))
			return (int)k;
	}
	return -1;
}

// The colour added onto itself, a unorm channel held at 1.
static void doubled_color(const Format *f, float out[CHANNELS])
{
	const bool unorm = f->kind == KIND_BYTES || f->kind == KIND_10_10_10_2;
	for (uint32_t c = 0; c < CHANNELS; c++)
		out[c] = unorm && f->color[c] > 0.5f ? 1.0f : 2.0f * f->color[c];
}

static void log_texel(const Format *f, const char *label, const char *what, uint32_t x, uint32_t y, const float *want)
{
	float got[CHANNELS] = { 0.0f };
	for (uint32_t c = 0; c < f->channels; c++)
		got[c] = texel_channel(f, x, y, c);
	if (want)
		xsdk_log_write("msaa: %s: %s at (%u, %u) is %g %g %g %g, not %g %g %g %g", label, what, (unsigned)x,
			(unsigned)y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
	else
		xsdk_log_write("msaa: %s: %s at (%u, %u) is %g %g %g %g", label, what, (unsigned)x, (unsigned)y, got[0], got[1],
			got[2], got[3]);
}

static float edge_side(Point a, Point b, float x, float y)
{
	return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

static void find_places(void)
{
	const float area = edge_side(triangle[0], triangle[1], triangle[2].x, triangle[2].y);
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			if (x >= REGION_LEFT) {
				places[y][x] = y < REGION_SPLIT ? PLACE_QUAD : PLACE_BLEND;
				continue;
			}
			bool inside = true;
			bool edge = false;
			for (uint32_t i = 0; i < TRIANGLE_VERTICES; i++) {
				const Point a = triangle[i];
				const Point b = triangle[(i + 1) % TRIANGLE_VERTICES];
				const float side = edge_side(a, b, x + 0.5f, y + 0.5f);
				const float length = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
				inside = inside && side * area > 0.0f;
				edge = edge || fabsf(side) <= EDGE_DISTANCE * length;
			}
			places[y][x] = edge ? PLACE_EDGE : inside ? PLACE_INSIDE : PLACE_OUTSIDE;
		}
	}
}

typedef struct {
	// Edge pixels at k of the samples' blend.
	uint32_t levels[MAX_SAMPLES + 1];
	uint32_t between;
	// Pixels off the edges not as drawn.
	uint32_t wrong;
	uint32_t doubled;
	uint32_t blend_texels;
} Tally;

// Every texel of the scene's texture: edge pixels by the blend they hold, the rest as drawn, and with blended the
// bottom quad doubled or not; unblended, nothing was drawn there.
static void tally_scene(const Format *f, uint32_t samples, bool blended, const char *label, Tally *t)
{
	float doubled[CHANNELS];
	doubled_color(f, doubled);
	uint32_t logged = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t where = places[y][x];
			if (where == PLACE_BLEND && blended) {
				t->blend_texels++;
				t->doubled += texel_near(f, x, y, doubled);
				continue;
			}
			if (where == PLACE_EDGE) {
				const int level = texel_level(f, x, y, samples);
				if (level >= 0) {
					t->levels[level]++;
					continue;
				}
				t->between++;
				if (logged++ < MAX_LOGGED)
					log_texel(f, label, "an edge pixel between steps", x, y, NULL);
				continue;
			}
			float want[CHANNELS];
			level_color(f, where == PLACE_INSIDE || where == PLACE_QUAD ? samples : 0, samples, want);
			if (texel_near(f, x, y, want))
				continue;
			t->wrong++;
			if (logged++ < MAX_LOGGED)
				log_texel(f, label, "a pixel off the edges", x, y, want);
		}
	}
}

// Every pixel off the edges as drawn, and each edge pixel one step of the samples' blend, every step between seen.
static void check_scene(uint32_t samples, const char *label, const Tally *t)
{
	char counts[64];
	uint32_t length = 0;
	bool every_step = true;
	for (uint32_t k = 0; k <= samples; k++) {
		length +=
			(uint32_t)snprintf(counts + length, sizeof counts - length, "%s%u", k ? ", " : "", (unsigned)t->levels[k]);
		every_step = every_step && (k == 0 || k == samples || t->levels[k] > 0);
	}
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "%s: drawn, every pixel off the edges as drawn: %u wrong", label, (unsigned)t->wrong);
	verdict(t->wrong == 0, text);
	snprintf(text, sizeof text, "%s: resolved, edge pixels by samples covered, 0 to %u: %s; %u between steps", label,
		(unsigned)samples, counts, (unsigned)t->between);
	verdict(every_step && t->between == 0, text);
}

static void check_blend(const Format *f, const char *label, const Tally *t)
{
	log_texel(f, label, "the quad blended onto itself", REGION_LEFT, REGION_SPLIT, NULL);
	const bool all = t->doubled == t->blend_texels;
	const char *const outcome = all ? "doubled" : t->doubled ? "partly doubled" : "not doubled";
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "%s: blended, %u of %u texels doubled, %s; at 1 sample milestone 25 found them %s",
		label, (unsigned)t->doubled, (unsigned)t->blend_texels, outcome, f->blends ? "doubled" : "not doubled");
	verdict(all == f->blends && (all || !t->doubled), text);
}

static void set_format_target(const Format *f, uint32_t samples)
{
	const XsdkColorTarget target = { .format = f->target, .tile = 0 };
	xsdk_probe_msaa_targets(WIDTH, HEIGHT, &target, 1, depth_tile_for(samples), samples);
}

// A resolve's clear; the triangle and the top quad in the format's colour; the bottom quad drawn, then added onto
// itself; then a resolve into the texture.
static bool draw_format_scene(const Format *f, uint32_t samples, const XsdkTexture *texture)
{
	set_format_target(f, samples);
	xsdk_gpu_resolve_target(0, texture, f->clear);
	set_format_target(f, samples);
	xsdk_gpu_set_shaders(&vertex_shader, &flat_shader);
	xsdk_gpu_set_pixel_constants(0, f->color, 1);
	draw_shape(TRIANGLE_FIRST, TRIANGLE_VERTICES, 0.0f);
	draw_rect(REGION_LEFT, 0, WIDTH, REGION_SPLIT, WIDTH, HEIGHT, 0.0f, SHAPE_ARGB);
	draw_rect(REGION_LEFT, REGION_SPLIT, WIDTH, HEIGHT, WIDTH, HEIGHT, 0.0f, SHAPE_ARGB);
	xsdk_gpu_set_blend(&additive_blend);
	draw_rect(REGION_LEFT, REGION_SPLIT, WIDTH, HEIGHT, WIDTH, HEIGHT, 0.0f, SHAPE_ARGB);
	xsdk_gpu_resolve_target(0, texture, f->clear);
	return wait_gpu("a format's scene");
}

// An 8_8_8_8 target at tile `at` cleared to the sentinel, then the format's target at tile 0 drawn whole; how many of
// the neighbour's pixels the draw changed.
static bool neighbour_changed(const Format *f, uint32_t samples, uint32_t at, uint32_t *changed)
{
	const uint32_t depth = depth_tile_for(samples);
	xsdk_gpu_set_target_samples_at(WIDTH, HEIGHT, samples, at, depth);
	xsdk_gpu_resolve(scratch_address, WIDTH, HEIGHT, SENTINEL_ARGB);
	set_format_target(f, samples);
	xsdk_gpu_set_shaders(&vertex_shader, &flat_shader);
	xsdk_gpu_set_pixel_constants(0, f->color, 1);
	draw_rect(0, 0, WIDTH, HEIGHT, WIDTH, HEIGHT, 0.0f, SHAPE_ARGB);
	xsdk_gpu_set_target_samples_at(WIDTH, HEIGHT, samples, at, depth);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, SENTINEL_ARGB);
	if (!wait_gpu("a neighbouring target's resolve"))
		return false;
	*changed = 0;
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			*changed += pixel_argb(x, y, WIDTH) != SENTINEL_ARGB;
	return true;
}

// The count holds if a neighbour right after the target keeps every pixel and one at half the count loses some.
static bool check_tiles(const Format *f, uint32_t samples, const char *label)
{
	const uint32_t tiles = format_tiles(f, samples);
	xsdk_log_write(
		"msaa: %s: a neighbouring target at tile %u, then at %u", label, (unsigned)tiles, (unsigned)(tiles / 2));
	uint32_t beside = 0;
	uint32_t overlapping = 0;
	if (!neighbour_changed(f, samples, tiles, &beside) || !neighbour_changed(f, samples, tiles / 2, &overlapping))
		return false;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text,
		"%s: takes %u eDRAM tiles: a target at tile %u kept its pixels, %u changed; one at tile %u lost %u of %u",
		label, (unsigned)tiles, (unsigned)tiles, (unsigned)beside, (unsigned)(tiles / 2), (unsigned)overlapping,
		(unsigned)(WIDTH * HEIGHT));
	verdict(beside == 0 && overlapping > 0, text);
	return true;
}

static bool run_format(const Format *f, uint32_t samples)
{
	char label[LABEL_BYTES];
	snprintf(label, sizeof label, "formats at %u: %s", (unsigned)samples, f->name);
	xsdk_log_write("msaa: %s: the scene, at tile 0, depth at tile %u", label, (unsigned)depth_tile_for(samples));
	XsdkTexture texture = { .texels = NULL };
	if (!NT_SUCCESS(xsdk_texture_create(&texture, f->texture, WIDTH, HEIGHT, 1, true))) {
		xsdk_log_write("msaa: %s: FAILED, no texture", label);
		return false;
	}
	const bool drawn = draw_format_scene(f, samples, &texture);
	if (drawn) {
		xsdk_texture_download_rows(&texture, 0, 0, HEIGHT, readback, WIDTH * texel_bytes(f));
		Tally tally;
		memset(&tally, 0, sizeof tally);
		tally_scene(f, samples, true, label, &tally);
		check_scene(samples, label, &tally);
		check_blend(f, label, &tally);
	}
	xsdk_texture_free(&texture);
	return drawn && check_tiles(f, samples, label);
}

static bool run_formats(uint32_t samples)
{
	bool ran = true;
	for (uint32_t i = 0; i < FORMATS && ran; i++)
		ran = run_format(&formats[i], samples);
	return ran;
}

// The four cleared, the triangle and the top quad drawn in each one's colour at once, then each resolved.
static bool draw_four_scene(uint32_t samples, const XsdkColorTarget *targets, const XsdkTexture *textures)
{
	float colors[XSDK_COLOR_TARGETS][CHANNELS];
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		memcpy(colors[i], formats[four_formats[i]].color, sizeof colors[i]);
	const uint32_t depth = depth_tile_for(samples);
	xsdk_probe_msaa_targets(WIDTH, HEIGHT, targets, XSDK_COLOR_TARGETS, depth, samples);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_gpu_resolve_target(i, &textures[i], formats[four_formats[i]].clear);
	xsdk_probe_msaa_targets(WIDTH, HEIGHT, targets, XSDK_COLOR_TARGETS, depth, samples);
	xsdk_gpu_set_shaders(&vertex_shader, &four_shader);
	xsdk_gpu_set_pixel_constants(0, &colors[0][0], XSDK_COLOR_TARGETS);
	draw_shape(TRIANGLE_FIRST, TRIANGLE_VERTICES, 0.0f);
	draw_rect(REGION_LEFT, 0, WIDTH, REGION_SPLIT, WIDTH, HEIGHT, 0.0f, SHAPE_ARGB);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_gpu_resolve_target(i, &textures[i], formats[four_formats[i]].clear);
	return wait_gpu("four targets' scene");
}

static bool run_four(uint32_t samples)
{
	XsdkColorTarget targets[XSDK_COLOR_TARGETS];
	XsdkTexture textures[XSDK_COLOR_TARGETS] = { { .texels = NULL } };
	uint32_t tile = 0;
	bool made = true;
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		const Format *const f = &formats[four_formats[i]];
		targets[i] = (XsdkColorTarget){ .format = f->target, .tile = tile };
		tile += format_tiles(f, samples);
		made = NT_SUCCESS(xsdk_texture_create(&textures[i], f->texture, WIDTH, HEIGHT, 1, true)) && made;
	}
	xsdk_log_write("msaa: four at %u: targets at tiles %u, %u, %u and %u, depth at %u; the scene", (unsigned)samples,
		(unsigned)targets[0].tile, (unsigned)targets[1].tile, (unsigned)targets[2].tile, (unsigned)targets[3].tile,
		(unsigned)depth_tile_for(samples));
	const bool drawn = made && draw_four_scene(samples, targets, textures);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS && drawn; i++) {
		const Format *const f = &formats[four_formats[i]];
		char label[LABEL_BYTES];
		snprintf(label, sizeof label, "four at %u: target %u, %s", (unsigned)samples, (unsigned)i, f->name);
		xsdk_texture_download_rows(&textures[i], 0, 0, HEIGHT, readback, WIDTH * texel_bytes(f));
		Tally tally;
		memset(&tally, 0, sizeof tally);
		tally_scene(f, samples, false, label, &tally);
		check_scene(samples, label, &tally);
	}
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		if (textures[i].texels)
			xsdk_texture_free(&textures[i]);
	}
	if (!made)
		xsdk_log_write("msaa: four at %u: FAILED, no textures", (unsigned)samples);
	return drawn;
}

// xsdk's averaging select at the count (src/gpu_draw.c, copy_sample_selects).
static uint32_t average_select(uint32_t samples)
{
	return samples == 4 ? SELECT_0123 : SELECT_01;
}

// Which k of steps the pixel blends, k steps the shape's colour and the rest the clear colour, or -1 for none.
static int blend_level(uint32_t argb, uint32_t steps)
{
	for (uint32_t k = 0; k <= steps; k++) {
		bool matches = true;
		for (uint32_t shift = 0; shift <= 24; shift += 8) {
			const int clear = CLEAR_ARGB >> shift & 0xFF;
			const int shape = SHAPE_ARGB >> shift & 0xFF;
			const int want = clear + (int)k * (shape - clear) / (int)steps;
			const int got = argb >> shift & 0xFF;
			matches = matches && got - want <= 1 && want - got <= 1;
		}
		if (matches)
			return (int)k;
	}
	return -1;
}

// What a sweep found: where it steps from all of a pixel's blend to none, if it does so once, and whether any pixel
// held some of the shape.
typedef struct {
	int step_at;
	bool any;
} Sweep;

// Logs where the row's or column's blend changes, as level@place, as milestone 32's probe did.
static Sweep sweep_colour(const char *label, bool along_row, uint32_t steps)
{
	const uint32_t count = along_row ? WIDTH : HEIGHT;
	char runs[TEXT_BYTES];
	uint32_t length = 0;
	uint32_t changes = 0;
	int last = -2;
	Sweep found = { .step_at = -1, .any = false };
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t argb = along_row ? pixel_argb(i, SWEEP_ROW, WIDTH) : pixel_argb(SWEEP_COLUMN, i, WIDTH);
		const int level = blend_level(argb, steps);
		found.any = found.any || level != 0;
		if (level == last)
			continue;
		if (last == (int)steps && level == 0)
			found.step_at = (int)i;
		changes++;
		last = level;
		if (length < sizeof runs - 16)
			length += (uint32_t)snprintf(runs + length, sizeof runs - length, " %d@%u", level, (unsigned)i);
	}
	xsdk_log_write("msaa: %s: %s sweep:%s", label, along_row ? "row 64" : "column 128", runs);
	if (changes != 2)
		found.step_at = -1;
	return found;
}

// A sample sits d 16ths of a pixel down from the centre where the rising edge's step lies at WIDTH / 2 + d * WIDTH /
// 16, and d across where the moving one's lies at HEIGHT / 2 + d * HEIGHT / 16; INT32_MIN off that grid.
static int offset_down(int step_at)
{
	const int from_middle = step_at - WIDTH / 2;
	return step_at >= 0 && from_middle % (WIDTH / SIXTEENTHS) == 0 ? from_middle / (WIDTH / SIXTEENTHS) : INT32_MIN;
}

static int offset_across(int step_at)
{
	const int from_middle = step_at - HEIGHT / 2;
	return step_at >= 0 && from_middle % (HEIGHT / SIXTEENTHS) == 0 ? from_middle / (HEIGHT / SIXTEENTHS) : INT32_MIN;
}

// A clear, then the sweep's shape drawn through the AA mask and resolved through the copy sample select alone.
static bool draw_sweep(uint32_t samples, uint32_t mask, uint32_t select, bool along_row, const char *label)
{
	xsdk_log_write("msaa: %s: sending", label);
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, samples);
	xsdk_gpu_resolve(scratch_address, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, samples);
	put_register(PA_SC_AA_MASK, mask);
	xsdk_gpu_set_shaders(&vertex_shader, &flat_shader);
	xsdk_gpu_set_pixel_constants(0, shape_color, 1);
	draw_shape(along_row ? ACROSS_FIRST : DOWN_FIRST, QUAD_VERTICES, 0.0f);
	xsdk_probe_msaa_select(select);
	// The resolve sets the mask back to all (src/gpu_draw.c, put_surface).
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_probe_msaa_select(UINT32_MAX);
	return wait_gpu(label);
}

// Milestone 32's offsets in 16ths of a pixel, down and across alike (docs/HARDWARE.md, "Multisampled targets,
// milestone 32").
static const int known_offsets[2][MAX_SAMPLES] = { { -4, 4 }, { -6, -2, 2, 6 } };

// The bits of known_offsets the offset is.
static uint32_t known_bit(uint32_t samples, int offset)
{
	for (uint32_t i = 0; i < samples; i++) {
		if (known_offsets[samples == 4][i] == offset)
			return 1u << i;
	}
	return 0;
}

// Sweeps by mask, 0 for all and m + 1 for sample m's bit, then by select, the last averaging, then row and column.
typedef struct {
	Sweep sweeps[MAX_SAMPLES + 1][MAX_SAMPLES + 1][2];
} PatternRun;

static void report_places(uint32_t samples, const PatternRun *run)
{
	uint32_t down_seen = 0;
	uint32_t across_seen = 0;
	bool clean = true;
	for (uint32_t s = 0; s < samples; s++) {
		const int down = offset_down(run->sweeps[0][s][0].step_at);
		const int across_16 = offset_across(run->sweeps[0][s][1].step_at);
		if (down == INT32_MIN || across_16 == INT32_MIN) {
			clean = false;
			xsdk_log_write("msaa: pattern at %u: copy sample select %u alone: no single step on the 16ths' grid",
				(unsigned)samples, (unsigned)s);
			continue;
		}
		xsdk_log_write(
			"msaa: pattern at %u: copy sample select %u sits %d/16 of a pixel across and %d/16 down from its "
			"centre",
			(unsigned)samples, (unsigned)s, across_16, down);
		down_seen |= known_bit(samples, down);
		across_seen |= known_bit(samples, across_16);
	}
	const uint32_t all = (1u << samples) - 1;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text,
		"pattern at %u: each select alone copies one sample, together milestone 32's places down and across",
		(unsigned)samples);
	verdict(clean && down_seen == all && across_seen == all, text);
}

// Each mask bit should reach one select, a different one each.
static void report_masks(uint32_t samples, const PatternRun *run)
{
	uint32_t selects_reached = 0;
	bool one_each = true;
	for (uint32_t m = 0; m < samples; m++) {
		uint32_t reached = 0;
		uint32_t select = 0;
		for (uint32_t s = 0; s < samples; s++) {
			if (run->sweeps[m + 1][s][0].any || run->sweeps[m + 1][s][1].any) {
				reached++;
				select = s;
			}
		}
		one_each = one_each && reached == 1 && !(selects_reached & 1u << select);
		selects_reached |= reached == 1 ? 1u << select : 0;
		xsdk_log_write("msaa: pattern at %u: AA mask %04X reached %u of the selects alone%s%u", (unsigned)samples,
			(unsigned)(AA_MASK_NIBBLES << m), (unsigned)reached, reached == 1 ? ", select " : ", the last ",
			(unsigned)select);
	}
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "pattern at %u: each AA mask bit lets one sample through, each a select of its own",
		(unsigned)samples);
	verdict(one_each, text);
}

static bool run_pattern(uint32_t samples)
{
	static PatternRun run;
	memset(&run, 0, sizeof run);
	for (uint32_t m = 0; m <= samples; m++) {
		const uint32_t mask = m ? AA_MASK_NIBBLES << (m - 1) : AA_MASK_ALL;
		for (uint32_t s = 0; s <= samples; s++) {
			const uint32_t select = s < samples ? s : average_select(samples);
			for (uint32_t shape = 0; shape < 2; shape++) {
				char label[LABEL_BYTES];
				snprintf(label, sizeof label, "pattern at %u: AA mask %04X, copy sample select %u, the %s edge",
					(unsigned)samples, (unsigned)mask, (unsigned)select, shape ? "moving" : "rising");
				if (!draw_sweep(samples, mask, select, shape == 0, label))
					return false;
				run.sweeps[m][s][shape] = sweep_colour(label, shape == 0, s < samples ? 1 : samples);
			}
		}
	}
	report_places(samples, &run);
	report_masks(samples, &run);
	return true;
}

// Logs the depth along the row or column as runs: n where the word holds the drawn depth, f the clear's, else the
// depth itself; and where it steps from n to f, if it does so once.
static int sweep_depth(const char *label, const XsdkTexture *texture, bool along_row)
{
	const uint32_t drawn = (uint32_t)(DEPTH_DRAWN * (double)DEPTH_24_MAX + 0.5);
	const uint32_t count = along_row ? WIDTH : HEIGHT;
	char runs[TEXT_BYTES];
	uint32_t length = 0;
	uint32_t changes = 0;
	uint32_t last = UINT32_MAX;
	int step_at = -1;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t x = along_row ? i : SWEEP_COLUMN;
		const uint32_t y = along_row ? SWEEP_ROW : i;
		const uint32_t depth = texture->texels[tiled_index(x, y, texture->pitch)] >> 8;
		const uint32_t near_drawn = depth > drawn ? depth - drawn : drawn - depth;
		// 0 near, 1 far, else the depth, kept apart from those two by being 2 more.
		const uint32_t kind = near_drawn <= DEPTH_24_TOLERANCE ? 0
			: depth >= DEPTH_24_MAX - DEPTH_24_TOLERANCE       ? 1
															   : depth + 2;
		if (kind == last)
			continue;
		if (last == 0 && kind == 1)
			step_at = (int)i;
		changes++;
		last = kind;
		if (length >= sizeof runs - 16)
			continue;
		if (kind < 2)
			length += (uint32_t)snprintf(runs + length, sizeof runs - length, " %c@%u", kind ? 'f' : 'n', (unsigned)i);
		else
			length += (uint32_t)snprintf(runs + length, sizeof runs - length, " %06X@%u", (unsigned)depth, (unsigned)i);
	}
	xsdk_log_write("msaa: %s: %s sweep:%s", label, along_row ? "row 64" : "column 128", runs);
	return changes == 2 ? step_at : -1;
}

// A clear to far, the sweep's shape drawn at DEPTH_DRAWN over every sample, its depth resolved through the select.
static bool draw_depth_sweep(
	uint32_t samples, uint32_t select, bool along_row, const XsdkTexture *texture, const char *label)
{
	xsdk_log_write("msaa: %s: sending", label);
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, samples);
	xsdk_gpu_resolve(scratch_address, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, samples);
	xsdk_gpu_set_shaders(&vertex_shader, &flat_shader);
	xsdk_gpu_set_pixel_constants(0, shape_color, 1);
	xsdk_gpu_set_depth_stencil(&always_write_state);
	draw_shape(along_row ? ACROSS_FIRST : DOWN_FIRST, QUAD_VERTICES, DEPTH_DRAWN);
	xsdk_probe_msaa_select(select);
	xsdk_gpu_resolve_depth_texture(texture);
	xsdk_probe_msaa_select(UINT32_MAX);
	return wait_gpu(label);
}

// Xenia copies one sample of depth whatever the select: 01 and 0123 as 0, 23 as 2 (xenia:src/xenia/gpu/draw_util.cc:
// 717-756).
static uint32_t xenia_depth_select(uint32_t select)
{
	return select == SELECT_23 ? 2 : select >= SELECT_01 ? 0 : select;
}

// Which sample each select copies, by the place its steps give; an averaging select either takes one, as Xenia's
// choice, or averages, the steps' runs then holding depths between.
static void report_depth(uint32_t samples, const uint32_t *selects, uint32_t count, int steps[][2])
{
	for (uint32_t i = 0; i < count; i++) {
		const int down = offset_down(steps[i][0]);
		const int across_16 = offset_across(steps[i][1]);
		const bool one = down != INT32_MIN && across_16 != INT32_MIN;
		if (one)
			xsdk_log_write("msaa: depth at %u: copy sample select %u copies the sample %d/16 across and %d/16 down",
				(unsigned)samples, (unsigned)selects[i], across_16, down);
		else
			xsdk_log_write(
				"msaa: depth at %u: copy sample select %u copies no one sample; its sweeps above hold what "
				"it copied",
				(unsigned)samples, (unsigned)selects[i]);
		char text[TEXT_BYTES];
		if (selects[i] < SELECT_01) {
			snprintf(text, sizeof text, "depth at %u: copy sample select %u copies one sample", (unsigned)samples,
				(unsigned)selects[i]);
			verdict(one, text);
			continue;
		}
		const uint32_t xenia = xenia_depth_select(selects[i]);
		const bool as_xenia = one && steps[i][0] == steps[xenia][0] && steps[i][1] == steps[xenia][1];
		snprintf(text, sizeof text, "depth at %u: copy sample select %u copies what select %u does, as Xenia has it",
			(unsigned)samples, (unsigned)selects[i], (unsigned)xenia);
		verdict(as_xenia, text);
	}
}

static bool run_depth(uint32_t samples)
{
	XsdkTexture texture = { .texels = NULL };
	if (!NT_SUCCESS(xsdk_texture_create(&texture, XSDK_FORMAT_D24S8, WIDTH, HEIGHT, 1, true))) {
		xsdk_log_write("msaa: depth at %u: FAILED, no texture", (unsigned)samples);
		return false;
	}
	// Each sample alone, then the averaging ones.
	uint32_t selects[MAX_SAMPLES + 3];
	uint32_t count = 0;
	for (uint32_t s = 0; s < samples; s++)
		selects[count++] = s;
	selects[count++] = SELECT_01;
	if (samples == 4) {
		selects[count++] = SELECT_23;
		selects[count++] = SELECT_0123;
	}
	int steps[MAX_SAMPLES + 3][2];
	bool ran = true;
	for (uint32_t i = 0; i < count && ran; i++) {
		for (uint32_t shape = 0; shape < 2 && ran; shape++) {
			char label[LABEL_BYTES];
			snprintf(
				label, sizeof label, "depth at %u: copy sample select %u", (unsigned)samples, (unsigned)selects[i]);
			ran = draw_depth_sweep(samples, selects[i], shape == 0, &texture, label);
			if (ran)
				steps[i][shape] = sweep_depth(label, &texture, shape == 0);
		}
	}
	if (ran)
		report_depth(samples, selects, count, steps);
	xsdk_texture_free(&texture);
	return ran;
}

// The hierarchical depth case's count, at 1280x256.
static uint32_t wide_samples;

enum {
	HIDDEN_QUADS = 16,
	// RB_HIZCONTROL's test bit (docs/HARDWARE.md, "Hierarchical depth, milestone 30").
	HIZ_TEST = 2,
	// Hidden quads count as rejected this many times as fast as drawn; milestone 30's were 84.8 times.
	REJECTED_SPEED_UP = 8,
	// Milestone 30's small rectangle, moved up into the 256 rows.
	SMALL_X = 400,
	SMALL_Y = 28,
	SMALL_SIZE = 200,
	// A full-screen draw's first block, which went wrong without the pass (milestone 49).
	BLOCK_WIDTH = 32,
	BLOCK_HEIGHT = 8,
	SCATTERED = 64,
	SCATTERED_MOST_WIDTH = 400,
	SCATTERED_MOST_HEIGHT = 200,
	SCATTERED_LOGGED = 8,
};

typedef struct {
	const char *name;
	float clear_depth;
	uint32_t compare;
	float near_depth;
	float far_depth;
} Direction;

static const Direction directions[] = {
	{ "less-than over a clear to 1", 1.0f, XSDK_COMPARE_LESS, 0.25f, 0.75f },
	{ "greater-than over a clear to 0", 0.0f, XSDK_COMPARE_GREATER, 0.75f, 0.25f },
};

static void set_wide_target(void)
{
	xsdk_gpu_set_target_samples(WIDE_WIDTH, WIDE_HEIGHT, wide_samples);
}

static uint32_t wide_pixels_not(uint32_t argb)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < WIDE_HEIGHT; y++)
		for (uint32_t x = 0; x < WIDE_WIDTH; x++)
			wrong += !near_argb(pixel_argb(x, y, WIDE_WIDTH), argb, 1);
	return wrong;
}

static uint64_t clock_span(void)
{
	return xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clocks);
}

// Milestone 30's probe (tests/probe_hiz.c): a clear, the near quad, then the hidden ones through the long shader
// between two copies of the GPU's clock; their clocks, or 0 if the GPU didn't finish.
static uint64_t time_hidden(const Direction *d, bool hiz_on, uint32_t *wrong)
{
	xsdk_log_write(
		"msaa: hiz at %u: %s, the buffer %s: sending", (unsigned)wide_samples, d->name, hiz_on ? "on" : "off");
	xsdk_gpu_set_depth_clear(d->clear_depth, 0);
	set_wide_target();
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, WIDE_HEIGHT, CLEAR_ARGB);
	set_wide_target();
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	xsdk_gpu_set_pixel_constants(0, zero, 1);
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = d->compare };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
	if (!hiz_on)
		put_register(RB_HIZCONTROL, 0);
	xsdk_gpu_set_shaders(&vertex_shader, &colour_shader);
	draw_rect(0, 0, WIDE_WIDTH, WIDE_HEIGHT, WIDE_WIDTH, WIDE_HEIGHT, d->near_depth, NEAR_ARGB);
	xsdk_gpu_set_shaders(&vertex_shader, &long_shader);
	place_rect(0, 0, WIDE_WIDTH, WIDE_HEIGHT, WIDE_WIDTH, WIDE_HEIGHT, d->far_depth, FAR_ARGB);
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks), true);
	for (uint32_t i = 0; i < HIDDEN_QUADS; i++)
		draw_quad();
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks) + CLOCK_BYTES, true);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, WIDE_HEIGHT, CLEAR_ARGB);
	if (!wait_gpu("hidden quads"))
		return 0;
	*wrong = wide_pixels_not(NEAR_ARGB);
	const uint64_t taken = clock_span();
	return taken ? taken : 1;
}

static bool check_hidden(void)
{
	for (uint32_t i = 0; i < sizeof directions / sizeof directions[0]; i++) {
		const Direction *const d = &directions[i];
		uint32_t wrong_on = 0;
		uint32_t wrong_off = 0;
		const uint64_t on = time_hidden(d, true, &wrong_on);
		const uint64_t off = on ? time_hidden(d, false, &wrong_off) : 0;
		if (!off)
			return false;
		char text[TEXT_BYTES];
		snprintf(text, sizeof text,
			"hiz at %u: %s: %u hidden quads took %u clocks with the buffer on and %u off, %u.%u times as fast; pixels "
			"not the near quad's colour: %u on, %u off",
			(unsigned)wide_samples, d->name, (unsigned)HIDDEN_QUADS, (unsigned)on, (unsigned)off, (unsigned)(off / on),
			(unsigned)(off * 10 / on % 10), (unsigned)wrong_on, (unsigned)wrong_off);
		verdict(on * REJECTED_SPEED_UP < off && !wrong_on && !wrong_off, text);
	}
	return true;
}

// Milestone 49's probe (tests/probe_hiz_pass.c) from here: a resolve clearing colour, and depth and the buffer.
static void clear_depth(float depth)
{
	xsdk_gpu_set_depth_clear(depth, 0);
	set_wide_target();
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, WIDE_HEIGHT, CLEAR_ARGB);
	(void)wait_gpu("a depth clear");
}

// Milestone 30's draw: the rectangle at depth, less-than without writing, testing the buffer whichever side the clear
// took.
static void draw_probe(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float depth)
{
	set_wide_target();
	xsdk_gpu_set_shaders(&vertex_shader, &colour_shader);
	xsdk_gpu_set_depth_stencil(&less_state);
	put_register(RB_HIZCONTROL, HIZ_TEST);
	draw_rect(x, y, x + w, y + h, WIDE_WIDTH, WIDE_HEIGHT, depth, PROBE_ARGB);
}

typedef struct {
	uint32_t wrong;
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;
} Wrong;

// Draws over the rectangle after the clears before, reads it back through a resolve that leaves depth and the buffer
// alone, and counts its pixels that aren't as the depth test has them: drawn everywhere if shown, else nowhere.
static bool count_wrong(
	const char *name, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, float depth, bool shown, Wrong *found)
{
	*found = (Wrong){ .left = WIDE_WIDTH, .top = WIDE_HEIGHT };
	if (stopped)
		return false;
	draw_probe(x0, y0, w, h, depth);
	xsdk_gpu_set_resolve_clears(true, false);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, WIDE_HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_resolve_clears(true, true);
	if (!wait_gpu(name))
		return false;
	for (uint32_t y = y0; y < y0 + h; y++) {
		for (uint32_t x = x0; x < x0 + w; x++) {
			// Red, green and blue; alpha isn't drawn apart from the clear's.
			const bool drawn = near_argb(pixel_argb(x, y, WIDE_WIDTH) | 0xFF000000, PROBE_ARGB, 1);
			if (drawn == shown)
				continue;
			found->wrong++;
			found->left = x < found->left ? x : found->left;
			found->top = y < found->top ? y : found->top;
			found->right = x > found->right ? x : found->right;
			found->bottom = y > found->bottom ? y : found->bottom;
		}
	}
	return true;
}

static void check_draw(const char *name, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, float depth, bool shown)
{
	Wrong found;
	if (!count_wrong(name, x0, y0, w, h, depth, shown, &found))
		return;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "hiz at %u: %s: %u of %u pixels wrong, in (%u, %u) to (%u, %u)", (unsigned)wide_samples,
		name, (unsigned)found.wrong, (unsigned)(w * h), (unsigned)found.left, (unsigned)found.top,
		(unsigned)found.right, (unsigned)found.bottom);
	verdict(found.wrong == 0, text);
}

// Milestone 30's sequences after a clear: a draw testing a clear to 0.25 loads the buffer's old value, which without
// the pass hid pixels of the next clear's first draw.
static void check_after_clears(void)
{
	clear_depth(1.0f);
	clear_depth(0.25f);
	check_draw(
		"cleared to 0.25 from far, a full-screen draw at 0.4 hidden", 0, 0, WIDE_WIDTH, WIDE_HEIGHT, 0.4f, false);
	clear_depth(1.0f);
	check_draw("cleared far from 0.25, a full-screen draw at 0.9 shown", 0, 0, WIDE_WIDTH, WIDE_HEIGHT, 0.9f, true);
	clear_depth(1.0f);
	clear_depth(0.25f);
	check_draw("cleared to 0.25 from far, a 200x200 draw at 0.4 hidden", SMALL_X, SMALL_Y, SMALL_SIZE, SMALL_SIZE, 0.4f,
		false);
	clear_depth(1.0f);
	check_draw(
		"cleared far from 0.25, a 200x200 draw at 0.9 shown", SMALL_X, SMALL_Y, SMALL_SIZE, SMALL_SIZE, 0.9f, true);
}

// Milestone 30's small sequence at the rectangle: a draw at 0.4 over a clear to 0.25 from far, then one at 0.9 over a
// clear to far, whose wrong pixels it counts.
static bool wrong_after_clears(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Wrong *shown)
{
	Wrong hidden;
	clear_depth(1.0f);
	clear_depth(0.25f);
	(void)count_wrong("a draw at 0.4", x, y, w, h, 0.4f, false, &hidden);
	clear_depth(1.0f);
	return count_wrong("a draw at 0.9", x, y, w, h, 0.9f, true, shown);
}

// Rectangles whose first block, their top right, lies in the top two rows of blocks, where the pass is drawn.
static const uint32_t top_rows[][4] = {
	{ 0, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 2 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 19 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 20 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ WIDE_WIDTH - 2 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ WIDE_WIDTH - BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 0, 0, 2 * BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 0, 0, WIDE_WIDTH / 2, BLOCK_HEIGHT },
	{ 0, 0, 2 * BLOCK_WIDTH, 100 },
	{ 0, 0, 1, 1 },
	{ 2 * BLOCK_WIDTH - 1, BLOCK_HEIGHT - 1, 1, 1 },
	{ WIDE_WIDTH - 1, 0, 1, 1 },
	{ WIDE_WIDTH - 1, BLOCK_HEIGHT - 1, 1, 1 },
	{ 0, BLOCK_HEIGHT, WIDE_WIDTH, BLOCK_HEIGHT },
	{ WIDE_WIDTH - BLOCK_WIDTH, BLOCK_HEIGHT, BLOCK_WIDTH, BLOCK_HEIGHT },
};

static void check_top_rows(void)
{
	uint32_t wrong_rects = 0;
	for (uint32_t i = 0; i < sizeof top_rows / sizeof top_rows[0] && !stopped; i++) {
		const uint32_t *const r = top_rows[i];
		Wrong shown;
		if (!wrong_after_clears(r[0], r[1], r[2], r[3], &shown))
			break;
		if (!shown.wrong)
			continue;
		wrong_rects++;
		xsdk_log_write("msaa: hiz at %u: top rows %ux%u at (%u, %u): %u pixels wrong, in (%u, %u) to (%u, %u)",
			(unsigned)wide_samples, (unsigned)r[2], (unsigned)r[3], (unsigned)r[0], (unsigned)r[1],
			(unsigned)shown.wrong, (unsigned)shown.left, (unsigned)shown.top, (unsigned)shown.right,
			(unsigned)shown.bottom);
	}
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "hiz at %u: %u rectangles starting in the top rows after clears: %u with wrong pixels",
		(unsigned)wide_samples, (unsigned)(sizeof top_rows / sizeof top_rows[0]), (unsigned)wrong_rects);
	verdict(!stopped && !wrong_rects, text);
}

// Milestone 30's small sequence at rectangles of every size and place, from milestone 49's seed.
static void check_scattered(void)
{
	uint32_t seed = 49;
	uint32_t wrong_rects = 0;
	uint32_t wrong_pixels = 0;
	for (uint32_t i = 0; i < SCATTERED && !stopped; i++) {
		seed = seed * 1664525 + 1013904223;
		const uint32_t w = 1 + (seed >> 8) % SCATTERED_MOST_WIDTH;
		seed = seed * 1664525 + 1013904223;
		const uint32_t h = 1 + (seed >> 8) % SCATTERED_MOST_HEIGHT;
		seed = seed * 1664525 + 1013904223;
		const uint32_t x = (seed >> 8) % (WIDE_WIDTH - w + 1);
		seed = seed * 1664525 + 1013904223;
		const uint32_t y = (seed >> 8) % (WIDE_HEIGHT - h + 1);
		Wrong shown;
		if (!wrong_after_clears(x, y, w, h, &shown))
			break;
		if (!shown.wrong)
			continue;
		if (wrong_rects++ < SCATTERED_LOGGED)
			xsdk_log_write("msaa: hiz at %u: scattered %ux%u at (%u, %u): %u pixels wrong, in (%u, %u) to (%u, %u)",
				(unsigned)wide_samples, (unsigned)w, (unsigned)h, (unsigned)x, (unsigned)y, (unsigned)shown.wrong,
				(unsigned)shown.left, (unsigned)shown.top, (unsigned)shown.right, (unsigned)shown.bottom);
		wrong_pixels += shown.wrong;
	}
	char text[TEXT_BYTES];
	snprintf(text, sizeof text,
		"hiz at %u: %u rectangles scattered after clears: %u with wrong pixels, %u pixels in all",
		(unsigned)wide_samples, (unsigned)SCATTERED, (unsigned)wrong_rects, (unsigned)wrong_pixels);
	verdict(!stopped && wrong_rects == 0, text);
}

static bool run_hiz(uint32_t samples)
{
	wide_samples = samples;
	const bool timed = check_hidden();
	if (timed) {
		xsdk_log_write("msaa: hiz at %u: milestone 49's draws after clears", (unsigned)samples);
		check_after_clears();
		check_scattered();
		check_top_rows();
	}
	xsdk_gpu_set_depth_clear(1.0f, 0);
	return timed && !stopped;
}

enum {
	FILL_QUADS = 64,
	RESOLVE_FRAMES = 8,
	BAND_ROWS = 256,
	// Milestone 31's channel tolerance.
	RATE_TOLERANCE = 3,
};

// Milestone 31's fill colour (tests/probe_pixel_rate.c).
static const float fill_color[CHANNELS] = { 0.25f, 0.5f, 0.75f, 0.5f };
// Points every rate resolve holds, within its 208 rows.
static const uint32_t rate_points[][2] = { { 0, 0 }, { 640, 128 }, { 1279, 207 }, { 300, 200 }, { 637, 127 } };

static uint32_t packed_argb(const float color[CHANNELS])
{
	const uint32_t a = (uint32_t)(color[3] * 255.0f + 0.5f);
	const uint32_t r = (uint32_t)(color[0] * 255.0f + 0.5f);
	const uint32_t g = (uint32_t)(color[1] * 255.0f + 0.5f);
	const uint32_t b = (uint32_t)(color[2] * 255.0f + 0.5f);
	return a << 24 | r << 16 | g << 8 | b;
}

static bool points_near(uint32_t argb)
{
	for (uint32_t i = 0; i < sizeof rate_points / sizeof rate_points[0]; i++) {
		if (!near_argb(pixel_argb(rate_points[i][0], rate_points[i][1], WIDE_WIDTH), argb, RATE_TOLERANCE))
			return false;
	}
	return true;
}

// Microseconds, in hundredths, from the GPU's clocks.
static uint32_t centi_us(uint64_t clocks_taken)
{
	return (uint32_t)(clocks_taken * 400 / CLOCKS_IN_4_US);
}

// Milestone 31's fills, full-target quads through a one-instruction shader writing c0, opaque or alpha blended.
static bool time_fill(uint32_t samples, bool blended)
{
	const char *const what = blended ? "blended fill" : "opaque fill";
	xsdk_log_write("msaa: rates at %u: %s: sending", (unsigned)samples, what);
	xsdk_gpu_set_target_samples(WIDE_WIDTH, WIDE_HEIGHT, samples);
	xsdk_gpu_set_shaders(&vertex_shader, &flat_shader);
	xsdk_gpu_set_pixel_constants(0, fill_color, 1);
	xsdk_gpu_set_blend(blended ? &alpha_blend : &opaque_blend);
	place_rect(0, 0, WIDE_WIDTH, WIDE_HEIGHT, WIDE_WIDTH, WIDE_HEIGHT, 0.0f, SHAPE_ARGB);
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks), true);
	for (uint32_t i = 0; i < FILL_QUADS; i++)
		draw_quad();
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks) + CLOCK_BYTES, true);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, WIDE_HEIGHT, CLEAR_ARGB);
	if (!wait_gpu(what))
		return false;
	const uint64_t taken = clock_span() ? clock_span() : 1;
	const uint64_t pixels = (uint64_t)WIDE_WIDTH * WIDE_HEIGHT * FILL_QUADS;
	const uint32_t us = centi_us(taken) / 100 ? centi_us(taken) / 100 : 1;
	const uint32_t per_clock = (uint32_t)(pixels * 100 / taken);
	const uint32_t samples_per_clock = per_clock * samples;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text,
		"rates at %u: %s: %u 1280x256 quads in %u clocks, %u us: %u Mpixels/s, %u.%02u pixels and %u.%02u samples a "
		"clock; read back as filled",
		(unsigned)samples, what, (unsigned)FILL_QUADS, (unsigned)taken, (unsigned)us, (unsigned)(pixels / us),
		(unsigned)(per_clock / 100), (unsigned)(per_clock % 100), (unsigned)(samples_per_clock / 100),
		(unsigned)(samples_per_clock % 100));
	verdict(points_near(packed_argb(fill_color)), text);
	return true;
}

// 1280x720 resolved in three bands of 256, 256 and 208 rows, as at 4 samples its colour alone takes 2880 tiles; or,
// whole, at 1 and 2 samples, its depth on its colour's tiles, as only colour is cleared.
static bool time_resolves(uint32_t samples, bool whole)
{
	const char *const what = whole ? "720p resolved whole" : "720p resolved in 3 bands";
	xsdk_log_write("msaa: rates at %u: %s: sending", (unsigned)samples, what);
	xsdk_gpu_set_resolve_clears(true, false);
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks), true);
	for (uint32_t frame = 0; frame < RESOLVE_FRAMES; frame++) {
		if (whole) {
			xsdk_gpu_set_target_samples_at(WIDE_WIDTH, FRAME_HEIGHT, samples, 0, 0);
			xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, FRAME_HEIGHT, CLEAR_ARGB);
			continue;
		}
		for (uint32_t top = 0; top < FRAME_HEIGHT; top += BAND_ROWS) {
			const uint32_t rows = FRAME_HEIGHT - top < BAND_ROWS ? FRAME_HEIGHT - top : BAND_ROWS;
			xsdk_gpu_set_target_samples(WIDE_WIDTH, rows, samples);
			xsdk_gpu_resolve(xsdk_gpu_address(image), WIDE_WIDTH, rows, CLEAR_ARGB);
		}
	}
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks) + CLOCK_BYTES, true);
	xsdk_gpu_set_resolve_clears(true, true);
	if (!wait_gpu(what))
		return false;
	const uint32_t each = centi_us(clock_span()) / RESOLVE_FRAMES;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text,
		"rates at %u: %s, colour cleared: %u frames in %u clocks, %u.%02u us a frame; read back as the clear",
		(unsigned)samples, what, (unsigned)RESOLVE_FRAMES, (unsigned)clock_span(), (unsigned)(each / 100),
		(unsigned)(each % 100));
	verdict(points_near(CLEAR_ARGB), text);
	return true;
}

static bool run_rates(uint32_t samples)
{
	(void)samples;
	static const uint32_t counts[] = { 1, 2, 4 };
	bool ran = true;
	for (uint32_t i = 0; i < sizeof counts / sizeof counts[0] && ran; i++) {
		const uint32_t n = counts[i];
		ran =
			time_fill(n, false) && time_fill(n, true) && time_resolves(n, false) && (n == 4 || time_resolves(n, true));
	}
	return ran;
}

enum {
	// RB_COLORCONTROL (xenia:src/xenia/gpu/register_table.inc:538): alpha_to_mask_enable, and from bit 24 a 2-bit
	// offset for each pixel of a 2x2 quad (xenia:src/xenia/gpu/registers.h:682-731).
	RB_COLORCONTROL = 0x2202,
	ALPHA_TO_MASK_ENABLE = 1 << 4,
	ALPHA_TO_MASK_OFFSETS_SHIFT = 24,
	QUAD_PIXELS = 4,
	// Alphas 0 to 16 sixteenths, each a 16x16 square in a 32x32 cell, 8 cells a row, on even pixels so quads line up.
	ALPHA_STEPS = 16,
	ALPHA_CELL = 32,
	ALPHA_MARGIN = 8,
	ALPHA_SIZE = 16,
	ALPHA_CELLS_ACROSS = 8,
	// Alphas 0, 1/4, 1/2, 3/4 and 1.
	QUARTER_STEPS = ALPHA_STEPS / 4,
};

// Each pixel's samples covered, bit s from copy sample select s.
static uint8_t covered[HEIGHT][WIDTH];

typedef struct {
	// Each alpha's first quad: the samples each of its pixels covered, by quad_pixel.
	uint8_t masks[ALPHA_STEPS + 1][QUAD_PIXELS];
	// Pixels a select's resolve held neither the shape's colour nor the clear's at.
	uint32_t between;
	// Pixels of a square unlike its first quad's at their place, and pixels off the squares covered.
	uint32_t unlike;
	uint32_t outside;
	// Pixels where the averaging resolve's step isn't the selects' count.
	uint32_t averages_off;
} Coverage;

static uint32_t square_x(uint32_t step)
{
	return step % ALPHA_CELLS_ACROSS * ALPHA_CELL + ALPHA_MARGIN;
}

static uint32_t square_y(uint32_t step)
{
	return step / ALPHA_CELLS_ACROSS * ALPHA_CELL + ALPHA_MARGIN;
}

// The alpha step of the square over the pixel, or -1 off the squares.
static int alpha_step_at(uint32_t x, uint32_t y)
{
	const uint32_t step = y / ALPHA_CELL * ALPHA_CELLS_ACROSS + x / ALPHA_CELL;
	const uint32_t in_x = x - square_x(step);
	const uint32_t in_y = y - square_y(step);
	return step <= ALPHA_STEPS && in_x < ALPHA_SIZE && in_y < ALPHA_SIZE ? (int)step : -1;
}

// A pixel's place in its quad in the offsets' order, (0, 0), (0, 1), (1, 0) and (1, 1) as x and y, as Xenia picks the
// offset (xenia:src/xenia/gpu/dxbc_shader_translator_om.cc:1881-1894).
static uint32_t quad_pixel(uint32_t x, uint32_t y)
{
	return (x & 1) << 1 | (y & 1);
}

// A clear, then the squares drawn under RB_COLORCONTROL at control, set back to xsdk's before the resolve through the
// select.
static bool draw_alphas(uint32_t samples, uint32_t control, uint32_t select, const char *label)
{
	xsdk_log_write("msaa: %s: sending", label);
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, samples);
	xsdk_gpu_resolve(scratch_address, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, samples);
	xsdk_gpu_set_shaders(&vertex_shader, &flat_shader);
	put_register(RB_COLORCONTROL, control);
	for (uint32_t step = 0; step <= ALPHA_STEPS; step++) {
		const float color[CHANNELS] = { shape_color[0], shape_color[1], shape_color[2], (float)step / ALPHA_STEPS };
		const uint32_t x = square_x(step);
		const uint32_t y = square_y(step);
		xsdk_gpu_set_pixel_constants(0, color, 1);
		draw_rect(x, y, x + ALPHA_SIZE, y + ALPHA_SIZE, WIDTH, HEIGHT, 0.0f, SHAPE_ARGB);
	}
	put_register(RB_COLORCONTROL, COLOR_CONTROL);
	xsdk_probe_msaa_select(select);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_probe_msaa_select(UINT32_MAX);
	return wait_gpu(label);
}

// The pixel's step of the shape's colour over the clear's, alpha left out as the squares' differ.
static int rgb_level(uint32_t x, uint32_t y, uint32_t steps)
{
	return blend_level(pixel_argb(x, y, WIDTH) | 0xFF000000, steps);
}

static void read_sample(uint32_t sample, Coverage *c)
{
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const int level = rgb_level(x, y, 1);
			if (level < 0)
				c->between++;
			else
				covered[y][x] |= (uint8_t)(level << sample);
		}
	}
}

static void read_averages(uint32_t samples, Coverage *c)
{
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			c->averages_off += rgb_level(x, y, samples) != __builtin_popcount(covered[y][x]);
}

static void tally_alphas(Coverage *c)
{
	for (uint32_t step = 0; step <= ALPHA_STEPS; step++)
		for (uint32_t q = 0; q < QUAD_PIXELS; q++)
			c->masks[step][q] = covered[square_y(step) + (q & 1)][square_x(step) + (q >> 1)];
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const int step = alpha_step_at(x, y);
			if (step < 0)
				c->outside += covered[y][x] != 0;
			else
				c->unlike += covered[y][x] != c->masks[step][quad_pixel(x, y)];
		}
	}
}

// The squares drawn and resolved once through each sample's select, then through the averaging one.
static bool read_coverage(uint32_t samples, uint32_t control, const char *label, Coverage *c)
{
	memset(covered, 0, sizeof covered);
	memset(c, 0, sizeof *c);
	char step[LABEL_BYTES];
	for (uint32_t s = 0; s < samples; s++) {
		snprintf(step, sizeof step, "%s: copy sample select %u", label, (unsigned)s);
		if (!draw_alphas(samples, control, s, step))
			return false;
		read_sample(s, c);
	}
	if (samples > 1) {
		snprintf(step, sizeof step, "%s: the averaging select", label);
		if (!draw_alphas(samples, control, UINT32_MAX, step))
			return false;
		read_averages(samples, c);
	}
	tally_alphas(c);
	char text[TEXT_BYTES];
	snprintf(text, sizeof text,
		"%s: read back: %u pixels neither the shape nor the clear, %u unlike their square's first quad, %u off the "
		"squares covered, %u averaged off the selects' count",
		label, (unsigned)c->between, (unsigned)c->unlike, (unsigned)c->outside, (unsigned)c->averages_off);
	verdict(!c->between && !c->unlike && !c->outside && !c->averages_off, text);
	return true;
}

static void report_control(uint32_t samples, const char *label, const Coverage *c)
{
	uint32_t short_of_all = 0;
	for (uint32_t step = 0; step <= ALPHA_STEPS; step++)
		for (uint32_t q = 0; q < QUAD_PIXELS; q++)
			short_of_all += c->masks[step][q] != (1u << samples) - 1;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "%s: every sample covered at every alpha: %u of %u quad pixels not", label,
		(unsigned)short_of_all, (unsigned)((ALPHA_STEPS + 1) * QUAD_PIXELS));
	verdict(!short_of_all, text);
}

static uint32_t quad_count(const uint8_t masks[QUAD_PIXELS])
{
	uint32_t count = 0;
	for (uint32_t q = 0; q < QUAD_PIXELS; q++)
		count += (uint32_t)__builtin_popcount(masks[q]);
	return count;
}

// Each alpha's quad, its pixels in the offsets' order: the samples each covered, by select bit, and how many.
static void log_alphas(uint32_t samples, const char *label, const Coverage *c)
{
	for (uint32_t step = 0; step <= ALPHA_STEPS; step++) {
		const uint8_t *const m = c->masks[step];
		xsdk_log_write(
			"msaa: %s: alpha %2u/16: pixels (0,0) (0,1) (1,0) (1,1) cover samples %X %X %X %X, %u %u %u %u of %u; %u "
			"of the quad's %u",
			label, (unsigned)step, m[0], m[1], m[2], m[3], __builtin_popcount(m[0]), __builtin_popcount(m[1]),
			__builtin_popcount(m[2]), __builtin_popcount(m[3]), (unsigned)samples, (unsigned)quad_count(m),
			(unsigned)(QUAD_PIXELS * samples));
	}
}

// Each pixel's thresholds: the first alpha at which it covers n samples, for each n.
static void log_thresholds(uint32_t samples, const char *label, const Coverage *c)
{
	for (uint32_t q = 0; q < QUAD_PIXELS; q++) {
		char firsts[TEXT_BYTES];
		uint32_t length = 0;
		for (uint32_t n = 1; n <= samples; n++) {
			uint32_t step = 0;
			while (step <= ALPHA_STEPS && (uint32_t)__builtin_popcount(c->masks[step][q]) < n)
				step++;
			if (step > ALPHA_STEPS)
				length += (uint32_t)snprintf(firsts + length, sizeof firsts - length, ", %u never", (unsigned)n);
			else
				length += (uint32_t)snprintf(
					firsts + length, sizeof firsts - length, ", %u at %u/16", (unsigned)n, (unsigned)step);
		}
		xsdk_log_write("msaa: %s: pixel (%u,%u), the first alpha covering n samples: %s", label, (unsigned)(q >> 1),
			(unsigned)(q & 1), firsts + 2);
	}
}

// Whether coverage only grows with alpha, is alike over the quad, and is n/4 at alpha n/4, by pixel where n/4 of its
// samples is whole and over the quad.
static void report_shape(uint32_t samples, const char *label, const Coverage *c)
{
	uint32_t lost = 0;
	uint32_t uneven = 0;
	uint32_t pixels_off = 0;
	uint32_t pixels_whole = 0;
	uint32_t quads_off = 0;
	for (uint32_t step = 0; step <= ALPHA_STEPS; step++) {
		const uint8_t *const m = c->masks[step];
		bool even = true;
		for (uint32_t q = 0; q < QUAD_PIXELS; q++) {
			lost += step && (c->masks[step - 1][q] & ~m[q]);
			even = even && __builtin_popcount(m[q]) == __builtin_popcount(m[0]);
		}
		uneven += !even;
		const uint32_t quarters = step / QUARTER_STEPS;
		if (step % QUARTER_STEPS)
			continue;
		quads_off += quad_count(m) != quarters * samples;
		if (quarters * samples % 4)
			continue;
		for (uint32_t q = 0; q < QUAD_PIXELS; q++)
			pixels_off += (uint32_t)__builtin_popcount(m[q]) * 4 != quarters * samples;
		pixels_whole += QUAD_PIXELS;
	}
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "%s: coverage only grows with alpha: %u steps of a pixel lost a sample", label,
		(unsigned)lost);
	verdict(!lost, text);
	snprintf(text, sizeof text, "%s: symmetric, the quad's pixels cover as many samples: %u of %u alphas not", label,
		(unsigned)uneven, (unsigned)(ALPHA_STEPS + 1));
	verdict(!uneven, text);
	snprintf(text, sizeof text, "%s: alpha n/4 covers n/4 of each pixel's samples, where whole: %u of %u pixels not",
		label, (unsigned)pixels_off, (unsigned)pixels_whole);
	verdict(!pixels_off, text);
	snprintf(text, sizeof text, "%s: alpha n/4 covers n/4 of the quad's samples: %u of 5 alphas not", label,
		(unsigned)quads_off);
	verdict(!quads_off, text);
}

// The control first, xsdk's RB_COLORCONTROL, then alpha to mask on with the offsets for the quad's pixels in order.
static bool run_atoc(uint32_t samples, const uint32_t offsets[QUAD_PIXELS])
{
	Coverage c;
	char label[LABEL_BYTES];
	snprintf(label, sizeof label, "atoc at %u: alpha to mask off, the control", (unsigned)samples);
	if (!read_coverage(samples, COLOR_CONTROL, label, &c))
		return false;
	report_control(samples, label, &c);
	uint32_t control = (COLOR_CONTROL & ~(0xFFu << ALPHA_TO_MASK_OFFSETS_SHIFT)) | ALPHA_TO_MASK_ENABLE;
	for (uint32_t q = 0; q < QUAD_PIXELS; q++)
		control |= offsets[q] << (ALPHA_TO_MASK_OFFSETS_SHIFT + 2 * q);
	snprintf(label, sizeof label, "atoc at %u: offsets %u,%u,%u,%u, RB_COLORCONTROL %08X", (unsigned)samples,
		(unsigned)offsets[0], (unsigned)offsets[1], (unsigned)offsets[2], (unsigned)offsets[3], (unsigned)control);
	if (!read_coverage(samples, control, label, &c))
		return false;
	log_alphas(samples, label, &c);
	log_thresholds(samples, label, &c);
	report_shape(samples, label, &c);
	return true;
}

// Offsets 2 should make coverage symmetric, and libxenon's 3, 1, 0, 2 dither it (xenia:src/xenia/gpu/registers.h:
// 710-721).
static bool run_atoc_symmetric(uint32_t samples)
{
	static const uint32_t offsets[QUAD_PIXELS] = { 2, 2, 2, 2 };
	return run_atoc(samples, offsets);
}

static bool run_atoc_dithered(uint32_t samples)
{
	static const uint32_t offsets[QUAD_PIXELS] = { 3, 1, 0, 2 };
	return run_atoc(samples, offsets);
}

typedef struct {
	const char *name;
	// "default", or a fresh boot case's own name, so it runs alone.
	const char *group;
	const char *what;
	bool (*run)(uint32_t samples);
	uint32_t samples;
} ProbeCase;

static const ProbeCase cases[] = {
	{ "formats_1", "default", "each target format at 1 sample, the control: drawn, edges resolved, blended, tiles",
		run_formats, 1 },
	{ "formats_2", "default", "each target format at 2 samples", run_formats, 2 },
	{ "formats_4", "default", "each target format at 4 samples", run_formats, 4 },
	{ "four_1", "default", "four targets at once at 1 sample, the control, each read back by value", run_four, 1 },
	{ "four_2", "default", "four targets at once at 2 samples", run_four, 2 },
	{ "four_4", "default", "four targets at once at 4 samples", run_four, 4 },
	{ "rates", "default", "fill and blend rates and a 720p resolve at 1, 2 and 4 samples, by the GPU's clock",
		run_rates, 0 },
	{ "hiz_1", "default", "hidden pixels and draws after clears at 1280x256, 1 sample, the control", run_hiz, 1 },
	{ "hiz_2", "default", "the same with hierarchical depth on at 2 samples", run_hiz, 2 },
	{ "hiz_4", "default", "the same at 4 samples", run_hiz, 4 },
	// PA_SC_AA_MASK other than 0xFFFF and copy sample selects 1 to 3 are new.
	{ "pattern_2", "pattern_2", "each sample's place at 2 samples, by AA mask and single-sample resolves", run_pattern,
		2 },
	{ "pattern_4", "pattern_4", "each sample's place at 4 samples", run_pattern, 4 },
	// Depth resolves at 2 and 4 samples, under every select, are new.
	{ "depth_2", "depth_2", "which sample a depth resolve copies at 2 samples, under each select", run_depth, 2 },
	{ "depth_4", "depth_4", "which sample a depth resolve copies at 4 samples", run_depth, 4 },
	// RB_COLORCONTROL with alpha to mask on is new; each case draws its control, alpha to mask off, first.
	{ "atoc_sym_1", "atoc_sym_1", "alpha to coverage at 1 sample, offsets 2,2,2,2", run_atoc_symmetric, 1 },
	{ "atoc_sym_2", "atoc_sym_2", "alpha to coverage at 2 samples, offsets 2,2,2,2", run_atoc_symmetric, 2 },
	{ "atoc_sym_4", "atoc_sym_4", "alpha to coverage at 4 samples, offsets 2,2,2,2", run_atoc_symmetric, 4 },
	{ "atoc_dither_1", "atoc_dither_1", "alpha to coverage at 1 sample, offsets 3,1,0,2", run_atoc_dithered, 1 },
	{ "atoc_dither_2", "atoc_dither_2", "alpha to coverage at 2 samples, offsets 3,1,0,2", run_atoc_dithered, 2 },
	{ "atoc_dither_4", "atoc_dither_4", "alpha to coverage at 4 samples, offsets 3,1,0,2", run_atoc_dithered, 4 },
};

enum { CASES = sizeof cases / sizeof cases[0] };

// The case game:\msaa_case.txt names, or the first of the group it names; without it, or naming neither, the cases
// are listed and the default group runs.
static uint32_t start_case(void)
{
	FILE *const file = fopen(start_path, "r");
	char name[NAME_BYTES] = "";
	const bool read = file && fscanf(file, "%15s", name) == 1;
	if (file)
		(void)fclose(file);
	for (uint32_t i = 0; read && i < CASES; i++) {
		if (!strcmp(name, cases[i].name) || !strcmp(name, cases[i].group))
			return i;
	}
	xsdk_log_write(
		"msaa probe: %s names no case or group; write one in it, a case to run it and the rest of its "
		"group, a fresh boot case alone after a reboot:",
		start_path);
	for (uint32_t i = 0; i < CASES; i++)
		xsdk_log_write("msaa probe:   %s (%s): %s", cases[i].name,
			strcmp(cases[i].group, cases[i].name) ? cases[i].group : "fresh boot", cases[i].what);
	return 0;
}

static bool upload_vertices(void)
{
	float data[VERTICES][VERTEX_FLOATS];
	for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
		const float vertex[VERTEX_FLOATS] = { unit_square[i][0], unit_square[i][1], 0.0f, 1.0f };
		memcpy(data[SQUARE_FIRST + i], vertex, sizeof vertex);
	}
	const struct {
		uint32_t first;
		const Point *points;
		uint32_t count;
	} shapes[] = {
		{ TRIANGLE_FIRST, triangle, TRIANGLE_VERTICES },
		{ ACROSS_FIRST, across, QUAD_VERTICES },
		{ DOWN_FIRST, down, QUAD_VERTICES },
	};
	for (uint32_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
		for (uint32_t i = 0; i < shapes[s].count; i++) {
			const Point p = shapes[s].points[i];
			// Edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837).
			const float vertex[VERTEX_FLOATS] = { (p.x - 0.5f) / (WIDTH / 2.0f) - 1.0f,
				1.0f - (p.y - 0.5f) / (HEIGHT / 2.0f), 0.0f, 1.0f };
			memcpy(data[shapes[s].first + i], vertex, sizeof vertex);
		}
	}
	volatile uint32_t *const memory = xsdk_gpu_alloc(sizeof data);
	if (!memory)
		return false;
	uint32_t words[sizeof data / 4];
	memcpy(words, data, sizeof data);
	for (uint32_t i = 0; i < sizeof data / 4; i++)
		memory[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	vertices_address = xsdk_gpu_address(memory);
	xsdk_gpu_invalidate(vertices_address, sizeof data);
	return true;
}

static void make_states(void)
{
	const XsdkBlend opaque = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
		XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	const XsdkBlend additive = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ONE,
		XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	const XsdkBlend alpha = { XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
		XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&opaque_blend, &opaque);
	xsdk_gpu_blend_state(&additive_blend, &additive);
	xsdk_gpu_blend_state(&alpha_blend, &alpha);
	const XsdkDepthStencil less = { .depth_test = true, .depth_write = false, .depth_compare = XSDK_COMPARE_LESS };
	const XsdkDepthStencil always = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&less_state, &less);
	xsdk_gpu_depth_stencil_state(&always_write_state, &always);
}

static bool load_shaders(void)
{
	return NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_load(&flat_shader, microcode_pixel_shader, sizeof microcode_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&four_shader, four_targets_pixel_shader, sizeof four_targets_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&colour_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&long_shader, long_pixel_shader, sizeof long_pixel_shader));
}

static bool make_resources(void)
{
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDE_WIDTH, FRAME_HEIGHT));
	volatile uint32_t *const scratch = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	clocks = xsdk_gpu_alloc(2 * CLOCK_BYTES);
	if (!image || !scratch || !clocks || !upload_vertices() || !load_shaders()) {
		xsdk_log_write("msaa probe: FAILED, the memory or the shaders couldn't be made");
		return false;
	}
	scratch_address = xsdk_gpu_address(scratch);
	make_states();
	find_places();
	return true;
}

int main(void)
{
	const uint32_t first = start_case();
	const char *const group = cases[first].group;
	xsdk_log_write(
		"msaa probe: from case %s, group %s (%s names a case or group)", cases[first].name, group, start_path);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("msaa probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources())
		return 1;
	for (uint32_t i = first; i < CASES && !strcmp(cases[i].group, group); i++) {
		xsdk_log_write("msaa probe: case %s: %s", cases[i].name, cases[i].what);
		if (!cases[i].run(cases[i].samples)) {
			// The GPU may be wedged: no more GPU work, and the exit's xsdk_gpu_close may only time out.
			xsdk_log_write("msaa probe: FAILED, case %s stopped; %u PASS, %u DIFFERS before it", cases[i].name,
				(unsigned)passed, (unsigned)differed);
			return 1;
		}
	}
	status = xsdk_gpu_close();
	xsdk_log_write("msaa probe: done, %u PASS, %u DIFFERS; xsdk_gpu_close 0x%08X", (unsigned)passed, (unsigned)differed,
		(unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
