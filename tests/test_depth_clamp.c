#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

// Depth clamp (milestone 61): rectangles across the near plane and across the far plane, each drawn with clipping on
// and with depth clamp, their colour and depth read back; then depth clamp beside a clip plane, set in each order.
// Which pixels past a plane are drawn decides the checks; the depth written there is logged, for the console to say.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	CHANNEL_TOLERANCE = 1,
	CELL_WIDTH = 200,
	CELL_HEIGHT = 120,
	// The clip plane keeps the rows above this one, the middle of the second row's cells.
	PLANE_ROW = 460,
};

// Depth cleared to a half, so neither 0 nor 1 written past a plane reads as the clear.
static const float clear_depth = 0.5f;

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	// Depth at the left and right edges, w 1: across the near plane the left half is beyond it, across the far plane
	// the right half past it.
	float left_z;
	float right_z;
	uint32_t argb;
} Cell;

enum {
	NEAR_CLIPPED,
	NEAR_CLAMPED,
	FAR_CLIPPED,
	FAR_CLAMPED,
	PLANE_AFTER_CLAMP,
	CLIPPED_AFTER_PLANE,
	CELLS,
};

// Drawn in this order, the first with the raster state the target set leaves.
static const Cell cells[CELLS] = {
	[NEAR_CLIPPED] = { "near plane, clipping on", 80, 120, -0.5f, 0.5f, 0xFFE04040 },
	[NEAR_CLAMPED] = { "near plane, depth clamp", 360, 120, -0.5f, 0.5f, 0xFF40E040 },
	[FAR_CLIPPED] = { "far plane, clipping on", 640, 120, 0.5f, 1.5f, 0xFF4040E0 },
	[FAR_CLAMPED] = { "far plane, depth clamp", 920, 120, 0.5f, 1.5f, 0xFFE0E040 },
	[PLANE_AFTER_CLAMP] = { "near plane, depth clamp, then a clip plane", 80, 400, -0.5f, 0.5f, 0xFFE040E0 },
	[CLIPPED_AFTER_PLANE] = { "near plane, a clip plane, then clipping on", 360, 400, -0.5f, 0.5f, 0xFF40E0E0 },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

// The clip-space x of a pixel column's left edge and y of a row's top.
static float clip_x(uint32_t column)
{
	return (column - 0.5f) / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(uint32_t row)
{
	return 1.0f - (row - 0.5f) / (HEIGHT / 2.0f);
}

static float unorm(uint32_t argb, uint32_t shift)
{
	return (argb >> shift & 0xFF) / 255.0f;
}

// Each cell's two triangles: top left, top right, bottom left, then bottom left, top right, bottom right.
static void cell_vertices(float vertices[CELLS * QUAD_VERTICES][VERTEX_FLOATS])
{
	static const uint32_t corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
	for (uint32_t i = 0; i < CELLS; i++) {
		const Cell *const c = &cells[i];
		for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
			const uint32_t right = corners[v][0];
			const uint32_t bottom = corners[v][1];
			const float vertex[VERTEX_FLOATS] = { clip_x(c->x + right * CELL_WIDTH),
				clip_y(c->y + bottom * CELL_HEIGHT), right ? c->right_z : c->left_z, 1.0f, unorm(c->argb, 16),
				unorm(c->argb, 8), unorm(c->argb, 0), 1.0f };
			memcpy(vertices[i * QUAD_VERTICES + v], vertex, sizeof vertex);
		}
	}
}

// The shaders, the cells' vertices and the states they draw with.
typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	uint32_t vertices;
	XsdkDepthStencilState depth_always;
	XsdkRasterState clipped;
	XsdkRasterState clamped;
} Drawing;

static void draw_cell(const Drawing *d, uint32_t cell)
{
	xsdk_gpu_set_vertices(0, d->vertices + cell * QUAD_BYTES, QUAD_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// Depth clamp set before the resolve that clears, so the target set after must take it off, and the cells' own
// settings put it back; the plane is set between the last two cells, after depth clamp and before clipping on.
static void draw_cells(const Drawing *d, uint32_t clear_address)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_raster(&d->clamped);
	xsdk_gpu_resolve(clear_address, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(d->vertex, d->pixel);
	xsdk_gpu_set_depth_stencil(&d->depth_always);
	draw_cell(d, NEAR_CLIPPED);
	xsdk_gpu_set_raster(&d->clamped);
	draw_cell(d, NEAR_CLAMPED);
	xsdk_gpu_set_raster(&d->clipped);
	draw_cell(d, FAR_CLIPPED);
	xsdk_gpu_set_raster(&d->clamped);
	draw_cell(d, FAR_CLAMPED);
	const float planes[XSDK_CLIP_PLANES][4] = { [0] = { 0.0f, 1.0f, 0.0f, -clip_y(PLANE_ROW) } };
	xsdk_gpu_set_clip_planes(1, planes);
	draw_cell(d, PLANE_AFTER_CLAMP);
	xsdk_gpu_set_raster(&d->clipped);
	draw_cell(d, CLIPPED_AFTER_PLANE);
}

// What the resolves left: the colours, and the depth and stencil words.
typedef struct {
	volatile const uint32_t *pixels;
	volatile const uint32_t *depth;
	uint32_t depth_pitch;
} Readback;

// Whether a resolved pixel, its bytes B, G, R and A, is within a step of the colour.
static bool colour_near(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

// Whether the cell's colour landed at its pixel a quarter or three quarters across and down, logged with the depth
// word there and the depth the rectangle has at that pixel's centre.
static bool drawn_at(
	const Readback *r, uint32_t cell, const char *where, uint32_t quarters_across, uint32_t quarters_down)
{
	const Cell *const c = &cells[cell];
	const uint32_t x = c->x + quarters_across * CELL_WIDTH / 4;
	const uint32_t y = c->y + quarters_down * CELL_HEIGHT / 4;
	const uint32_t stored = r->pixels[tiled_index(x, y, WIDTH)];
	const uint32_t word = r->depth[tiled_index(x, y, r->depth_pitch)];
	const float z = c->left_z + (x - c->x + 0.5f) / CELL_WIDTH * (c->right_z - c->left_z);
	const bool drawn = colour_near(stored, c->argb);
	xsdk_log_write("depth clamp: %s, %s at (%u, %u): %s (%08X), depth %06X (%.4f) where the rectangle's z is %.4f",
		c->name, where, (unsigned)x, (unsigned)y, drawn ? "drawn" : "not drawn", (unsigned)stored,
		(unsigned)(word >> 8), (word >> 8) / (double)0xFFFFFF, z);
	return drawn;
}

static void check_cells(const Readback *r)
{
	xsdk_log_write("depth clamp: depth cleared to %.4f before the draws; 000000 is 0 and FFFFFF is 1", clear_depth);
	const bool near_kept = drawn_at(r, NEAR_CLIPPED, "in front of the plane", 3, 2);
	const bool near_past = drawn_at(r, NEAR_CLIPPED, "beyond it", 1, 2);
	check("depth clamp: off as the target is set, a rectangle across the near plane draws in front of it, not beyond",
		near_kept && !near_past);
	const bool clamped_kept = drawn_at(r, NEAR_CLAMPED, "in front of the plane", 3, 2);
	const bool clamped_past = drawn_at(r, NEAR_CLAMPED, "beyond it", 1, 2);
	check("depth clamp: on, a rectangle across the near plane draws beyond it too", clamped_kept && clamped_past);
	const bool far_kept = drawn_at(r, FAR_CLIPPED, "before the plane", 1, 2);
	const bool far_past = drawn_at(r, FAR_CLIPPED, "past it", 3, 2);
	check("depth clamp: off, a rectangle across the far plane draws before it, not past", far_kept && !far_past);
	const bool far_clamped_kept = drawn_at(r, FAR_CLAMPED, "before the plane", 1, 2);
	const bool far_clamped_past = drawn_at(r, FAR_CLAMPED, "past it", 3, 2);
	check("depth clamp: on, a rectangle across the far plane draws past it too", far_clamped_kept && far_clamped_past);
	const bool plane_kept = drawn_at(r, PLANE_AFTER_CLAMP, "in front of the near plane, above the clip plane", 3, 1);
	const bool plane_past = drawn_at(r, PLANE_AFTER_CLAMP, "beyond the near plane, above the clip plane", 1, 1);
	// Xenia clips by no user plane with clipping off (xenia:src/xenia/gpu/d3d12/pipeline_cache.cc:885-886).
	const bool plane_cut = !drawn_at(r, PLANE_AFTER_CLAMP, "in front of the near plane, below the clip plane", 3, 3);
	xsdk_log_write("depth clamp: with depth clamp on, the clip plane %s", plane_cut ? "cuts" : "cuts nothing");
	check("depth clamp: kept on by a clip plane set after it", plane_kept && plane_past);
	const bool after_kept = drawn_at(r, CLIPPED_AFTER_PLANE, "in front of the near plane, above the clip plane", 3, 1);
	const bool after_past = drawn_at(r, CLIPPED_AFTER_PLANE, "beyond the near plane, above the clip plane", 1, 1);
	const bool after_cut = !drawn_at(r, CLIPPED_AFTER_PLANE, "in front of the near plane, below the clip plane", 3, 3);
	check("depth clamp: off again, a clip plane set before it still cuts, and the near plane clips",
		after_kept && !after_past && after_cut);
}

// Draws the cells, resolves their depth into a D24S8 texture and their colour into the buffer, and checks both.
static void run(const Drawing *d, volatile uint32_t *buffer, bool has_display)
{
	XsdkTexture depth;
	const bool made = NT_SUCCESS(xsdk_texture_create(&depth, XSDK_FORMAT_D24S8, WIDTH, HEIGHT, 1, true));
	check("depth clamp: a tiled D24S8 texture made", made);
	if (!made)
		return;
	const uint32_t address = xsdk_gpu_address(buffer);
	xsdk_gpu_set_depth_clear(clear_depth, 0);
	draw_cells(d, address);
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_depth_texture(&depth);
	xsdk_gpu_set_resolve_clears(true, true);
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	check("depth clamp: the resolves' fence landed", landed);
	if (landed) {
		const Readback readback = { .pixels = buffer, .depth = depth.texels, .depth_pitch = depth.pitch };
		check_cells(&readback);
	}
	if (landed && has_display) {
		check("depth clamp: the cells shown", NT_SUCCESS(xsdk_display_show(address, WIDTH)));
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
	xsdk_texture_free(&depth);
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

static void make_states(Drawing *d)
{
	const XsdkDepthStencil always = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&d->depth_always, &always);
	xsdk_gpu_raster_state(&d->clipped, &(const XsdkRaster){ .cull = XSDK_CULL_NONE });
	xsdk_gpu_raster_state(&d->clamped, &(const XsdkRaster){ .cull = XSDK_CULL_NONE, .depth_clamp = true });
}

void test_depth_clamp(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("depth clamp: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("depth clamp: shaders load, patched for a position and a colour", loaded);
	float vertices[CELLS * QUAD_VERTICES][VERTEX_FLOATS];
	cell_vertices(vertices);
	volatile uint32_t *const vertex_memory = upload(vertices, sizeof vertices);
	volatile uint32_t *const buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	check("depth clamp: vertex and colour buffers", vertex_memory && buffer);
	// Without a display, as in Xenia, the cells still draw and are checked.
	if (loaded && vertex_memory && buffer) {
		Drawing drawing = { .vertex = &vertex, .pixel = &pixel, .vertices = xsdk_gpu_address(vertex_memory) };
		make_states(&drawing);
		run(&drawing, buffer, has_display);
	}
	xsdk_display_close();
	xsdk_gpu_free(buffer);
	xsdk_gpu_free(vertex_memory);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("depth clamp: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
