#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsc_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/xsc.h"

// Constants read at run-time indices through xost's whole optimiser (docs/scope/48-xost-code-quality-2.md, step 6):
// vk_indexed_reads.vert, compiled on the console with the optimiser on, draws 64 by 36 quads in one draw, each its own
// i of 16 and j of 4. The optimiser reads most rows at a0 where they're used, and keeps offsets[j] past i's load and
// tints[i] past the interpolators' alloc, where a0 is lost (docs/HARDWARE.md, "a0 doesn't survive an alloc"), as
// copies. A quad's red and green name its i and its blue its j, m[j] moves it to its place, and offsets past j's count
// move it off for i, so a row read at the wrong a0 moves or recolours it; every quad's centre is checked every frame.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FRAMES = 30,
	FENCE_TIMEOUT_MS = 1000,
	MAX_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	// A colour converts once on the way to the 8-bit target (tests/test_instancing.c).
	TOLERANCE = 1,
	QUAD = 20,
	COLUMNS = WIDTH / QUAD,
	QUADS = COLUMNS * (HEIGHT / QUAD),
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	VERTEX_WORDS = QUADS * QUAD_VERTICES * VERTEX_FLOATS,
	I_COUNT = 16,
	J_COUNT = 4,
	// The block's rows: m[4] by columns, then tints, offsets and scales, 16 each.
	TINTS = 4 * J_COUNT,
	OFFSETS = TINTS + I_COUNT,
	SCALES = OFFSETS + I_COUNT,
	ROWS = SCALES + I_COUNT,
	// An offset's x and y past j's count, read only at the wrong a0: it moves a quad past the target.
	FAR = 8,
};

// A quad's two triangles, in units of a quad.
static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };

static const XsdkVertexElement layout[] = { { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 } };

typedef struct {
	XsdkXscShader compiled;
	XsdkShader vertex;
	XsdkShader pixel;
	float rows[ROWS][4];
	uint32_t block;
	volatile uint32_t *vertices;
	volatile uint32_t *image;
} Grid;

static uint32_t quad_i(uint32_t quad)
{
	return quad % I_COUNT;
}

// j a step on every 16 quads and every row, so neighbours differ.
static uint32_t quad_j(uint32_t quad)
{
	return (quad / I_COUNT + quad / COLUMNS) % J_COUNT;
}

static uint32_t red(uint32_t i)
{
	return 16 * i + 8;
}

static uint32_t blue(uint32_t j)
{
	return 64 * j + 32;
}

// m[j]'s move, in clip space.
static float move_x(uint32_t j)
{
	return 0.25f * (float)j;
}

static float move_y(uint32_t j)
{
	return -0.125f * (float)j;
}

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// What a resolve stores for a colour, alpha 255: its bytes blue, green, red, alpha from the top.
static uint32_t stored(uint32_t r, uint32_t g, uint32_t b)
{
	return b << 24 | g << 16 | r << 8 | 0xFF;
}

static bool close_to(uint32_t held, uint32_t want)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t a = held >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > TOLERANCE)
			return false;
	}
	return true;
}

// m[j] moves by its column 3; tints[i] * 2 is i's red and green, alpha 1; offsets[j] is j's blue and leaves the
// place alone, offsets[i] past j's count moves it; scales[j] is 1, and past j's count a half.
static void fill_rows(Grid *g)
{
	memset(g->rows, 0, sizeof g->rows);
	for (uint32_t j = 0; j < J_COUNT; j++) {
		for (uint32_t c = 0; c < 4; c++)
			g->rows[4 * j + c][c] = 1.0f;
		g->rows[4 * j + 3][0] = move_x(j);
		g->rows[4 * j + 3][1] = move_y(j);
	}
	for (uint32_t n = 0; n < I_COUNT; n++) {
		const float half_red = (float)red(n) / 255.0f / 2.0f;
		const float half_green = (float)(255 - red(n)) / 255.0f / 2.0f;
		const float tint[4] = { half_red, half_green, 0.0f, 0.5f };
		memcpy(g->rows[TINTS + n], tint, sizeof tint);
		const float offset[4] = { n < J_COUNT ? 0.0f : FAR, n < J_COUNT ? 0.0f : FAR,
			n < J_COUNT ? (float)blue(n) / 255.0f : 0.0f, 0.0f };
		memcpy(g->rows[OFFSETS + n], offset, sizeof offset);
		const float scale = n < J_COUNT ? 1.0f : 0.5f;
		const float scales[4] = { scale, scale, scale, scale };
		memcpy(g->rows[SCALES + n], scales, sizeof scales);
	}
}

// Each vertex: its corner, less m[j]'s move, then i and j. Pixel x's centre is at x (tests/test_instancing.c).
static void put_vertices(volatile uint32_t *vertices)
{
	for (uint32_t v = 0; v < QUADS * QUAD_VERTICES; v++) {
		const uint32_t quad = v / QUAD_VERTICES;
		const float x = (float)(quad % COLUMNS * QUAD) + corners[v % QUAD_VERTICES][0] * QUAD - 0.5f;
		const float y = (float)(quad / COLUMNS * QUAD) + corners[v % QUAD_VERTICES][1] * QUAD - 0.5f;
		const float vertex[VERTEX_FLOATS] = { x / (WIDTH / 2.0f) - 1.0f - move_x(quad_j(quad)),
			1.0f - y / (HEIGHT / 2.0f) - move_y(quad_j(quad)), (float)quad_i(quad), (float)quad_j(quad) };
		for (uint32_t k = 0; k < VERTEX_FLOATS; k++)
			vertices[v * VERTEX_FLOATS + k] = float_bits(vertex[k]);
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), VERTEX_WORDS * 4);
}

static const XscTestShader *xsc_shader(const char *name)
{
	for (size_t i = 0; i < sizeof xsc_test_shaders / sizeof xsc_test_shaders[0]; i++) {
		if (!strcmp(xsc_test_shaders[i].name, name))
			return &xsc_test_shaders[i];
	}
	return NULL;
}

// The shader compiled on the console as the raster test compiles its own, its block, its vertices and the target's
// buffer.
static bool make_grid(Grid *g)
{
	const XscTestShader *const source = xsc_shader("vk_indexed_reads.vert");
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	if (!source || xsdk_xsc_compile(source->spirv, source->spirv_words, &options, &g->compiled) != XSDK_XSC_OK ||
		g->compiled.resource_count != 1 ||
		!NT_SUCCESS(
			xsdk_shader_load(&g->vertex, (const uint32_t *)g->compiled.container, g->compiled.container_bytes)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&g->vertex, layout, 1, (const uint32_t[]){ STRIDE_BYTES })) ||
		!NT_SUCCESS(xsdk_shader_load(&g->pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)))
		return false;
	g->block = g->compiled.resources[0].reg;
	fill_rows(g);
	g->vertices = xsdk_gpu_alloc(VERTEX_WORDS * 4);
	g->image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!g->vertices || !g->image)
		return false;
	put_vertices(g->vertices);
	return true;
}

// One draw of every quad, resolved and waited for; false if the fence didn't land.
static bool draw(const Grid *g)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(&g->vertex, &g->pixel);
	for (size_t i = 0; i < g->compiled.need_count; i++) {
		const XsdkXscNeed *const n = &g->compiled.needs[i];
		const float value[4] = { (float)n->value[0], (float)n->value[1], (float)n->value[2], (float)n->value[3] };
		if (n->kind == XSDK_XSC_LITERAL)
			xsdk_gpu_set_vertex_constants(n->reg, value, 1);
	}
	xsdk_gpu_set_vertex_constants(g->block, &g->rows[0][0], ROWS);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(g->vertices), VERTEX_WORDS * 4);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUADS * QUAD_VERTICES);
	xsdk_gpu_resolve(xsdk_gpu_address(g->image), WIDTH, HEIGHT, CLEAR_ARGB);
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

// The quads whose centre isn't their i's red and green and their j's blue, the first few logged.
static uint32_t wrong_quads(const Grid *g, uint32_t frame, uint32_t *logged)
{
	uint32_t wrong = 0;
	for (uint32_t quad = 0; quad < QUADS; quad++) {
		const uint32_t x = quad % COLUMNS * QUAD + QUAD / 2;
		const uint32_t y = quad / COLUMNS * QUAD + QUAD / 2;
		const uint32_t i = quad_i(quad);
		const uint32_t want = stored(red(i), 255 - red(i), blue(quad_j(quad)));
		const uint32_t held = g->image[tiled_index(x, y, WIDTH)];
		if (close_to(held, want))
			continue;
		wrong++;
		if ((*logged)++ < MAX_LOGGED)
			xsdk_log_write("indexed reads: frame %u: quad %u, i %u, j %u, held %08X, not %08X", (unsigned)frame,
				(unsigned)quad, (unsigned)i, (unsigned)quad_j(quad), (unsigned)held, (unsigned)want);
	}
	return wrong;
}

static void free_grid(Grid *g)
{
	xsdk_gpu_free(g->image);
	xsdk_gpu_free(g->vertices);
	if (g->pixel.microcode)
		xsdk_shader_free(&g->pixel);
	if (g->vertex.microcode)
		xsdk_shader_free(&g->vertex);
	// The shader's fetch patch records point into the container (src/shader.c), so it goes last.
	xsdk_xsc_free(&g->compiled);
}

void test_indexed_reads(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("indexed reads: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	Grid g = { .vertex = { .microcode = NULL }, .pixel = { .microcode = NULL } };
	const bool made = make_grid(&g);
	check("indexed reads: xost compiles vk_indexed_reads.vert on the console, and its grid is made", made);
	// The first frame's colour is whatever eDRAM held; its resolve clears it.
	bool landed = made && draw(&g);
	uint32_t wrong = 0, logged = 0;
	for (uint32_t frame = 1; frame < FRAMES && landed; frame++) {
		landed = draw(&g);
		wrong += landed ? wrong_quads(&g, frame, &logged) : 0;
	}
	char name[256];
	(void)snprintf(name, sizeof name,
		"indexed reads: one draw of vk_indexed_reads.vert, the optimiser on, places and colours each of %u quads by "
		"rows at its own i and j, read at a0 where they're used: %u of %u quads wrong over %u frames",
		(unsigned)QUADS, (unsigned)wrong, (unsigned)(QUADS * (FRAMES - 1)), (unsigned)(FRAMES - 1));
	check(name, made && landed && wrong == 0);
	free_grid(&g);
	check("indexed reads: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
