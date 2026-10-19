#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "constant_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Resolving part of a target (vulkan4xenon milestone 27). A target of four coloured quarters is resolved a rectangle
// at a time into a texture of its size: one inside it, and one reaching its right and bottom edges, both off the
// 8-pixel grid there. The texture must hold the quarters in the rectangles alone, and a resolve of the whole after
// them each rectangle's clear alone. Then depth cleared far in a rectangle, after a clear of the whole nearer, must
// let a quad between them show there alone, so a part's clear can't leave hierarchical depth rejecting it. Last, more
// rectangles than xsdk keeps the vertices of (src/gpu.c, RECTANGLE_SLOTS) each resolve their own texels, and queuing
// them doesn't wait out the GPU in sleeps: Dusklight's passes waited 10 ms a frame with 8 kept.

enum {
	// Off the 8-pixel grid both ways, as the edge rectangle needs; the pitch is 224.
	WIDTH = 204,
	HEIGHT = 116,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	CHANNEL_TOLERANCE = 1,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	// What the textures start as, so a texel a resolve never wrote shows.
	SENTINEL_ARGB = 0xFF00FFFF,
	BACKGROUND_ARGB = 0xFF202020,
	INSIDE_CLEAR_ARGB = 0xFFE0E040,
	EDGE_CLEAR_ARGB = 0xFF40E0E0,
	QUAD_ARGB = 0xFFE040E0,
	RECTANGLES = 2,
	// 8x8 cells from the top left, a row of 25 at a time: more than xsdk keeps, each a place of its own.
	CELL = 8,
	CELLS = 96,
	CELLS_ACROSS = WIDTH / CELL,
	// Queuing the cells: 1.6 ms on the console with 64 kept, 21 ms with 8.
	CELLS_MOST_US = 10000,
};

// The quarters meet off the grid, at 100 and 58, so a rectangle copied a cell out of place shows.
enum {
	SPLIT_X = 100,
	SPLIT_Y = 58,
};

static const uint32_t quarter_argb[4] = { 0xFFE04040, 0xFF40E040, 0xFF4040E0, 0xFFE0A040 };

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t clear_argb;
} Rect;

static const Rect rects[RECTANGLES] = {
	{ "inside", 40, 16, 64, 48, INSIDE_CLEAR_ARGB },
	{ "at the edges", 152, 96, WIDTH - 152, HEIGHT - 96, EDGE_CLEAR_ARGB },
};

// Depth is cleared to 0.75 over the whole, then to 1 in the inside rectangle; the quad at 0.875 tests less-than.
static const float near_clear = 0.75f;
static const float far_clear = 1.0f;
static const float quad_depth = 0.875f;

static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static uint32_t image[HEIGHT][WIDTH];

// An R8G8B8A8 texel's word as the CPU reads it: its bytes red, green, blue then alpha.
static uint32_t rgba_word(uint32_t argb)
{
	return argb << 8 | argb >> 24;
}

static bool inside(const Rect *rect, uint32_t x, uint32_t y)
{
	return x >= rect->x && x < rect->x + rect->width && y >= rect->y && y < rect->y + rect->height;
}

static uint32_t quarter_at(uint32_t x, uint32_t y)
{
	return quarter_argb[(y >= SPLIT_Y) * 2 + (x >= SPLIT_X)];
}

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square, edges half a pixel early, at the depth in c1.z, coloured by c2.
static void draw_quad(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float depth, uint32_t argb)
{
	const float left = (x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (x + width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (y + height - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static bool make_texture(XsdkTexture *texture)
{
	if (!NT_SUCCESS(xsdk_texture_create(texture, XSDK_FORMAT_R8G8B8A8, WIDTH, HEIGHT, 1, true)))
		return false;
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			image[y][x] = rgba_word(SENTINEL_ARGB);
	xsdk_texture_upload(texture, 0, &image[0][0], sizeof image[0]);
	xsdk_gpu_invalidate(texture->address, texture->size_bytes);
	return true;
}

static bool near_word(uint32_t got, uint32_t want)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t a = got >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

typedef uint32_t (*Expected)(uint32_t x, uint32_t y);

// Every texel against what it should hold; the first few wrong ones logged.
static uint32_t count_wrong(const char *what, const XsdkTexture *texture, Expected expected)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t got = texture->texels[tiled_index(x, y, texture->pitch)];
			const uint32_t want = rgba_word(expected(x, y));
			if (near_word(got, want))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				xsdk_log_write("resolve rect: %s: (%u, %u) held %08X, not %08X", what, (unsigned)x, (unsigned)y,
					(unsigned)got, (unsigned)want);
			wrong++;
		}
	}
	return wrong;
}

// The rectangles' texture: the quarters inside them, the sentinel elsewhere.
static uint32_t copied_at(uint32_t x, uint32_t y)
{
	for (uint32_t i = 0; i < RECTANGLES; i++) {
		if (inside(&rects[i], x, y))
			return quarter_at(x, y);
	}
	return SENTINEL_ARGB;
}

// The whole target's resolve after them: each rectangle's clear, the quarters elsewhere.
static uint32_t cleared_at(uint32_t x, uint32_t y)
{
	for (uint32_t i = 0; i < RECTANGLES; i++) {
		if (inside(&rects[i], x, y))
			return rects[i].clear_argb;
	}
	return quarter_at(x, y);
}

// The cells' texture: the quarters in each cell resolved, the sentinel elsewhere.
static uint32_t cells_at(uint32_t x, uint32_t y)
{
	const uint32_t cell = y / CELL * CELLS_ACROSS + x / CELL;
	return x < CELLS_ACROSS * CELL && cell < CELLS ? quarter_at(x, y) : SENTINEL_ARGB;
}

static uint32_t depth_at(uint32_t x, uint32_t y)
{
	return inside(&rects[0], x, y) ? QUAD_ARGB : BACKGROUND_ARGB;
}

static bool landed(void)
{
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	uint32_t vertices;
	XsdkTexture *parts;
	XsdkTexture *whole;
	XsdkTexture *cells;
} Scene;

static void set_target(const Scene *scene)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	xsdk_gpu_set_vertices(0, scene->vertices, sizeof square);
}

static void draw_quarters(const Scene *scene)
{
	set_target(scene);
	for (uint32_t i = 0; i < 4; i++) {
		const uint32_t x = i % 2 ? SPLIT_X : 0;
		const uint32_t y = i / 2 ? SPLIT_Y : 0;
		draw_quad(x, y, i % 2 ? WIDTH - SPLIT_X : SPLIT_X, i / 2 ? HEIGHT - SPLIT_Y : SPLIT_Y, 0.0f, quarter_argb[i]);
	}
}

// The quarters, each rectangle resolved and cleared in turn, then the whole target.
static void check_colour(const Scene *scene)
{
	draw_quarters(scene);
	xsdk_gpu_set_resolve_clears(true, false);
	for (uint32_t i = 0; i < RECTANGLES; i++) {
		const Rect *const r = &rects[i];
		xsdk_gpu_resolve_texture_rect(scene->parts, r->x, r->y, r->width, r->height, r->clear_argb);
	}
	xsdk_gpu_resolve_texture(scene->whole, BACKGROUND_ARGB);
	xsdk_gpu_set_resolve_clears(true, true);
	const bool done = landed();
	check("resolve rect: the resolves' fence landed", done);
	const uint32_t copied = done ? count_wrong("rectangles", scene->parts, copied_at) : 1;
	const uint32_t cleared = done ? count_wrong("whole after them", scene->whole, cleared_at) : 1;
	xsdk_log_write("resolve rect: %u texels wrong in the rectangles' texture, %u in the whole's", (unsigned)copied,
		(unsigned)cleared);
	check(
		"resolve rect: rectangles inside a target and at its edges, off the 8-pixel grid there, resolve into the same "
		"texels and no others",
		copied == 0);
	check("resolve rect: each rectangle's resolve clears it alone", cleared == 0);
}

// Depth near over the whole, far in the inside rectangle, then a quad between them over the whole target.
static void check_depth(const Scene *scene)
{
	XsdkDepthStencilState less;
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	xsdk_gpu_depth_stencil_state(&less, &setting);
	xsdk_gpu_set_depth_clear(near_clear, 0);
	xsdk_gpu_resolve_texture(scene->whole, BACKGROUND_ARGB);
	xsdk_gpu_set_depth_clear(far_clear, 0);
	xsdk_gpu_set_resolve_clears(false, true);
	const Rect *const r = &rects[0];
	xsdk_gpu_resolve_texture_rect(scene->parts, r->x, r->y, r->width, r->height, 0);
	set_target(scene);
	xsdk_gpu_set_depth_stencil(&less);
	draw_quad(0, 0, WIDTH, HEIGHT, quad_depth, QUAD_ARGB);
	xsdk_gpu_set_resolve_clears(true, true);
	xsdk_gpu_resolve_texture(scene->whole, BACKGROUND_ARGB);
	xsdk_gpu_set_depth_clear(1.0f, 0);
	const bool done = landed();
	const uint32_t wrong = done ? count_wrong("depth", scene->whole, depth_at) : 1;
	xsdk_log_write("resolve rect: %u texels wrong after the rectangle's depth clear", (unsigned)wrong);
	check("resolve rect: a rectangle's depth clear clears its depth alone, and draws find it", done && wrong == 0);
}

// The quarters, then each cell resolved alone, clearing nothing, timed as the CPU queues them.
static void check_cells(const Scene *scene)
{
	draw_quarters(scene);
	xsdk_gpu_set_resolve_clears(false, false);
	const uint64_t start = now_us();
	for (uint32_t i = 0; i < CELLS; i++)
		xsdk_gpu_resolve_texture_rect(scene->cells, i % CELLS_ACROSS * CELL, i / CELLS_ACROSS * CELL, CELL, CELL, 0);
	const uint64_t queued_us = now_us() - start;
	xsdk_gpu_set_resolve_clears(true, true);
	const bool done = landed();
	const uint32_t wrong = done ? count_wrong("cells", scene->cells, cells_at) : 1;
	xsdk_log_write("resolve rect: %u cells queued in %llu us, %u texels wrong", (unsigned)CELLS,
		(unsigned long long)queued_us, (unsigned)wrong);
	check("resolve rect: 96 rectangles, more than xsdk keeps, queue without waiting out the GPU in sleeps",
		queued_us < CELLS_MOST_US);
	check("resolve rect: 96 rectangles, more than xsdk keeps, each resolve their own texels", done && wrong == 0);
}

static volatile uint32_t *upload(const void *data, uint32_t bytes)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(bytes);
	if (!vertices)
		return NULL;
	for (uint32_t i = 0; i < bytes / 4; i++) {
		uint32_t word;
		memcpy(&word, (const uint8_t *)data + i * 4, sizeof word);
		vertices[i] = word;
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), bytes);
	return vertices;
}

void test_resolve_rect(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("resolve rect: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	XsdkTexture parts = { .texels = NULL };
	XsdkTexture whole = { .texels = NULL };
	XsdkTexture cells = { .texels = NULL };
	volatile uint32_t *const vertices = upload(square, sizeof square);
	const bool made = loaded && vertices && make_texture(&parts) && make_texture(&whole) && make_texture(&cells);
	check("resolve rect: shaders, vertices and textures made", made);
	if (made) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.vertices = xsdk_gpu_address(vertices),
			.parts = &parts,
			.whole = &whole,
			.cells = &cells };
		check_colour(&scene);
		check_depth(&scene);
		check_cells(&scene);
	}
	xsdk_texture_free(&cells);
	xsdk_texture_free(&whole);
	xsdk_texture_free(&parts);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("resolve rect: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
