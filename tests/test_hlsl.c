#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "xsc_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"
#include "xsdk/xsc.h"

// D3D9's semantics through glslang's HLSL (docs/scope/50-hlsl.md, step 4): xost's grid_*.hlsl, compiled on the console
// with the optimiser on, each pixel shader drawn behind a vertex shader loaded for it and linked to it by semantic
// (xsdk_shader_link), as five cells where the raster test's frame has nothing, each checked by pixel. One vertex
// shader's TEXCOORD0 and COLOR0 meet a pixel shader's by semantic, with a TEXCOORD1 declared first; VPOS is each
// pixel's whole column and row; VFACE's sign on a clockwise and a counter-clockwise quad, front faces clockwise as on
// D3D9, is reported; DEPTH from TEXCOORD0's x leaves a nearer quad in front of half a cell; and another vertex shader's
// TEXCOORD3, COLOR1, TEXCOORD0 and FOG meet a pixel shader reading COLOR1 and TEXCOORD3 alone, in 2 interpolators.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 10,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	// A colour converts once on the way to the 8-bit target, a ramp across a quad a step either side of that.
	TOLERANCE = 1,
	RAMP_TOLERANCE = 2,
	// The cells, 80 by 40, in the column the raster test's frame leaves free right of its viewport cell.
	CELL_X = 960,
	CELL_WIDTH = 80,
	CELL_HEIGHT = 40,
	LINK_Y = 420,
	VPOS_Y = 465,
	VFACE_Y = 510,
	DEPTH_Y = 555,
	GAPS_Y = 600,
	// A position, a colour and a coordinate a vertex: POSITION, COLOR0 and TEXCOORD0.
	VERTEX_FLOATS = 10,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_VERTICES = 6,
	// The link's quad, VPOS's, VFACE's two, DEPTH's, the nearer one over it and the link with gaps'.
	QUADS = 7,
	VERTEX_WORDS = QUADS * QUAD_VERTICES * VERTEX_FLOATS,
	// VPOS's red and green: the column and row mod 64, times 4.
	VPOS_PERIOD = 64,
	VPOS_STEP = 4,
	// DEPTH's columns this far either side of the middle aren't checked, where its depth meets the nearer quad's.
	DEPTH_MARGIN = 4,
	// The link with gaps' pixel shader's interpolators, COLOR1's and TEXCOORD3's; a map of each semantic to one
	// interpolator in both stages, step 4's first, took 14.
	GAPS_INTERPOLATORS = 2,
};

// The vertex shaders, then the pixel shaders.
enum {
	SHADER_VERTEX,
	SHADER_GAPS_VERTEX,
	SHADER_LINK,
	SHADER_VPOS,
	SHADER_VFACE,
	SHADER_DEPTH,
	SHADER_GAPS,
	SHADERS,
};

static const char *const shader_names[SHADERS] = { "hlsl/grid_vs.hlsl", "hlsl/grid_gaps_vs.hlsl",
	"hlsl/grid_link_ps.hlsl", "hlsl/grid_vpos_ps.hlsl", "hlsl/grid_vface_ps.hlsl", "hlsl/grid_depth_ps.hlsl",
	"hlsl/grid_gaps_ps.hlsl" };

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 32 },
};

// The link's colour, red 64 and green 191, and the nearer quad's red.
static const float link_colour[4] = { 0.25f, 0.75f, 0.0f, 1.0f };
static const float near_colour[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

typedef struct {
	uint32_t y;
	// Its corners right to left, which makes it counter-clockwise on screen, a back face.
	bool mirrored;
	uint32_t x;
	uint32_t width;
	float depth;
	const float *colour;
} Quad;

static const Quad quads[QUADS] = {
	{ LINK_Y, false, CELL_X, CELL_WIDTH, 0.5f, link_colour },
	{ VPOS_Y, false, CELL_X, CELL_WIDTH, 0.5f, link_colour },
	{ VFACE_Y, false, CELL_X, CELL_WIDTH / 2, 0.5f, link_colour },
	{ VFACE_Y, true, CELL_X + CELL_WIDTH / 2, CELL_WIDTH / 2, 0.5f, link_colour },
	{ DEPTH_Y, false, CELL_X, CELL_WIDTH, 0.75f, link_colour },
	{ DEPTH_Y, false, CELL_X, CELL_WIDTH, 0.5f, near_colour },
	{ GAPS_Y, false, CELL_X, CELL_WIDTH, 0.5f, link_colour },
};

// A quad's two triangles, in units of a quad: top left, top right, bottom left, then the other half.
static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };

typedef struct {
	XsdkXscShader compiled[SHADERS];
	// The pixel shaders, and each one's vertex shader, loaded for it and linked to it.
	XsdkShader shaders[SHADERS];
	XsdkShader linked[SHADERS];
	volatile uint32_t *vertices;
	volatile uint32_t *buffers[BUFFERS];
	XsdkRasterState raster;
	XsdkDepthStencilState depth;
	bool has_display;
} Grid;

typedef struct {
	uint32_t link;
	uint32_t vpos;
	uint32_t depth;
	uint32_t gaps;
	// VFACE's front and back quads: pixels reading +1, -1 and neither.
	uint32_t faces[2][3];
} Wrong;

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static const XscTestShader *xsc_shader(const char *name)
{
	for (size_t i = 0; i < sizeof xsc_test_shaders / sizeof xsc_test_shaders[0]; i++) {
		if (!strcmp(xsc_test_shaders[i].name, name))
			return &xsc_test_shaders[i];
	}
	return NULL;
}

// Each vertex: its corner's clip-space place, the pixel x's centre at x (tests/test_indexed_reads.c), the quad's depth
// and colour, and TEXCOORD0's x from 0 at the quad's left edge to 1 at its right.
static void put_vertices(volatile uint32_t *vertices)
{
	for (uint32_t v = 0; v < QUADS * QUAD_VERTICES; v++) {
		const Quad *const q = &quads[v / QUAD_VERTICES];
		const float u = q->mirrored ? 1.0f - corners[v % QUAD_VERTICES][0] : corners[v % QUAD_VERTICES][0];
		const float x = (float)q->x + u * (float)q->width - 0.5f;
		const float y = (float)q->y + corners[v % QUAD_VERTICES][1] * CELL_HEIGHT - 0.5f;
		const float vertex[VERTEX_FLOATS] = { x / (WIDTH / 2.0f) - 1.0f, 1.0f - y / (HEIGHT / 2.0f), q->depth, 1.0f,
			q->colour[0], q->colour[1], q->colour[2], q->colour[3], u, 0.0f };
		for (uint32_t k = 0; k < VERTEX_FLOATS; k++)
			vertices[v * VERTEX_FLOATS + k] = float_bits(vertex[k]);
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), VERTEX_WORDS * 4);
}

static bool compile_shader(Grid *g, uint32_t i)
{
	const XscTestShader *const source = xsc_shader(shader_names[i]);
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	return source && xsdk_xsc_compile(source->spirv, source->spirv_words, &options, &g->compiled[i]) == XSDK_XSC_OK;
}

static NTSTATUS load(XsdkShader *shader, const XsdkXscShader *compiled)
{
	return xsdk_shader_load(shader, (const uint32_t *)compiled->container, compiled->container_bytes);
}

static uint32_t vertex_of(uint32_t pixel)
{
	return pixel == SHADER_GAPS ? SHADER_GAPS_VERTEX : SHADER_VERTEX;
}

// The pixel shader, and the vertex shader loaded for it and linked to it.
static bool load_pair(Grid *g, uint32_t pixel)
{
	const uint32_t vertex = vertex_of(pixel);
	return NT_SUCCESS(load(&g->shaders[pixel], &g->compiled[pixel])) &&
		NT_SUCCESS(load(&g->linked[pixel], &g->compiled[vertex])) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&g->linked[pixel], layout, 3, (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_link(&g->linked[pixel], &g->shaders[pixel]));
}

// The shaders compiled on the console as the raster test compiles its own, the vertices and the frames' buffers.
static bool make_grid(Grid *g)
{
	for (uint32_t i = 0; i < SHADERS; i++) {
		if (!compile_shader(g, i))
			return false;
	}
	for (uint32_t i = SHADER_LINK; i < SHADERS; i++) {
		if (!load_pair(g, i))
			return false;
	}
	xsdk_gpu_raster_state(&g->raster, &(XsdkRaster){ .cull = XSDK_CULL_NONE, .front_clockwise = true });
	xsdk_gpu_depth_stencil_state(
		&g->depth, &(XsdkDepthStencil){ .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS });
	g->vertices = xsdk_gpu_alloc(VERTEX_WORDS * 4);
	for (uint32_t i = 0; i < BUFFERS; i++)
		g->buffers[i] = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!g->vertices || !g->buffers[0] || !g->buffers[1])
		return false;
	put_vertices(g->vertices);
	return true;
}

// The shader's literals, into the stage's constants.
static void set_literals(const XsdkXscShader *compiled, bool pixel)
{
	for (size_t i = 0; i < compiled->need_count; i++) {
		const XsdkXscNeed *const n = &compiled->needs[i];
		const float value[4] = { (float)n->value[0], (float)n->value[1], (float)n->value[2], (float)n->value[3] };
		if (n->kind != XSDK_XSC_LITERAL)
			continue;
		if (pixel)
			xsdk_gpu_set_pixel_constants(n->reg, value, 1);
		else
			xsdk_gpu_set_vertex_constants(n->reg, value, 1);
	}
}

static void draw_quads(const Grid *g, uint32_t pixel, uint32_t first, uint32_t count)
{
	xsdk_gpu_set_shaders(&g->linked[pixel], &g->shaders[pixel]);
	set_literals(&g->compiled[vertex_of(pixel)], false);
	set_literals(&g->compiled[pixel], true);
	xsdk_gpu_set_vertices(
		0, xsdk_gpu_address(g->vertices) + first * QUAD_VERTICES * STRIDE_BYTES, count * QUAD_VERTICES * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, count * QUAD_VERTICES);
}

// One frame of the four cells, resolved and waited for; false if the fence didn't land.
static bool draw(const Grid *g, volatile uint32_t *buffer)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_raster(&g->raster);
	draw_quads(g, SHADER_LINK, 0, 1);
	draw_quads(g, SHADER_VPOS, 1, 1);
	draw_quads(g, SHADER_VFACE, 2, 2);
	draw_quads(g, SHADER_GAPS, 6, 1);
	xsdk_gpu_set_depth_stencil(&g->depth);
	draw_quads(g, SHADER_DEPTH, 4, 1);
	draw_quads(g, SHADER_LINK, 5, 1);
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

// What a resolve stores for a colour, alpha 255: its bytes blue, green, red, alpha from the top.
static uint32_t stored(uint32_t r, uint32_t g, uint32_t b)
{
	return b << 24 | g << 16 | r << 8 | 0xFF;
}

static bool close_to(uint32_t held, uint32_t want, uint32_t tolerance)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t a = held >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > tolerance)
			return false;
	}
	return true;
}

static uint32_t channel(uint32_t held, uint32_t shift)
{
	return held >> shift & 0xFF;
}

// TEXCOORD0's x at column x of a cell, at the pixel's centre, as 8 bits.
static uint32_t ramp(uint32_t x)
{
	return (uint32_t)(((float)(x - CELL_X) + 0.5f) / CELL_WIDTH * 255.0f + 0.5f);
}

static bool pixel_right(const char *what, uint32_t x, uint32_t y, uint32_t held, uint32_t want, uint32_t tolerance,
	uint32_t frame, uint32_t *logged)
{
	if (close_to(held, want, tolerance))
		return true;
	if ((*logged)++ < MAX_LOGGED)
		xsdk_log_write("hlsl: frame %u: %s at (%u, %u) held %08X, not %08X", (unsigned)frame, what, (unsigned)x,
			(unsigned)y, (unsigned)held, (unsigned)want);
	return false;
}

// 0 for +1, red 255; 1 for -1, red 0; 2 for neither, or a size other than 1, green 255, and blue a quarter.
static uint32_t face_sign(uint32_t held)
{
	const uint32_t red = channel(held, 8);
	if (!close_to(held & 0xFFFF00FF, stored(0, 255, 64), TOLERANCE))
		return 2;
	return red >= 255 - TOLERANCE ? 0 : red <= TOLERANCE ? 1 : 2;
}

static void check_frame(volatile const uint32_t *buffer, uint32_t frame, Wrong *wrong, uint32_t *logged)
{
	for (uint32_t dy = 0; dy < CELL_HEIGHT; dy++) {
		for (uint32_t x = CELL_X; x < CELL_X + CELL_WIDTH; x++) {
			uint32_t y = LINK_Y + dy;
			const uint32_t link = stored(64, 191, ramp(x));
			wrong->link +=
				!pixel_right("link", x, y, buffer[tiled_index(x, y, WIDTH)], link, RAMP_TOLERANCE, frame, logged);
			y = VPOS_Y + dy;
			const uint32_t vpos = stored(x % VPOS_PERIOD * VPOS_STEP, y % VPOS_PERIOD * VPOS_STEP, 0);
			wrong->vpos += !pixel_right("VPOS", x, y, buffer[tiled_index(x, y, WIDTH)], vpos, TOLERANCE, frame, logged);
			y = VFACE_Y + dy;
			wrong->faces[x >= CELL_X + CELL_WIDTH / 2][face_sign(buffer[tiled_index(x, y, WIDTH)])]++;
			y = DEPTH_Y + dy;
			const uint32_t middle = CELL_X + CELL_WIDTH / 2;
			if (x + DEPTH_MARGIN > middle && x < middle + DEPTH_MARGIN)
				continue;
			const uint32_t depth = x < middle ? stored(0, 255, 0) : stored(255, 0, ramp(x));
			wrong->depth +=
				!pixel_right("DEPTH", x, y, buffer[tiled_index(x, y, WIDTH)], depth, RAMP_TOLERANCE, frame, logged);
			y = GAPS_Y + dy;
			wrong->gaps +=
				!pixel_right("gaps", x, y, buffer[tiled_index(x, y, WIDTH)], link, RAMP_TOLERANCE, frame, logged);
		}
	}
}

static const char *sign_name(const uint32_t counts[3])
{
	if (counts[0] && !counts[1] && !counts[2])
		return "+1";
	if (counts[1] && !counts[0] && !counts[2])
		return "-1";
	return counts[2] ? "neither +1 nor -1" : "both +1 and -1";
}

static void report(const Wrong *w, bool landed, uint32_t frames)
{
	const uint32_t pixels = CELL_WIDTH * CELL_HEIGHT * frames;
	const uint32_t depth_pixels = (CELL_WIDTH - 2 * DEPTH_MARGIN + 1) * CELL_HEIGHT * frames;
	char name[320];
	(void)snprintf(name, sizeof name,
		"hlsl: a vertex shader's TEXCOORD0 and COLOR0 meet a pixel shader's by semantic, a TEXCOORD1 declared before "
		"them and the pixel shader's in the other order: %u of %u pixels wrong",
		(unsigned)w->link, (unsigned)pixels);
	check(name, landed && frames && w->link == 0);
	(void)snprintf(name, sizeof name, "hlsl: VPOS is each pixel's whole column and row: %u of %u pixels wrong",
		(unsigned)w->vpos, (unsigned)pixels);
	check(name, landed && frames && w->vpos == 0);
	const char *const front = sign_name(w->faces[0]);
	const char *const back = sign_name(w->faces[1]);
	(void)snprintf(name, sizeof name,
		"hlsl: VFACE is %s on a front face, clockwise, and %s on a back one, as D3D9's +1 and -1", front, back);
	check(name, landed && frames && !strcmp(front, "+1") && !strcmp(back, "-1"));
	(void)snprintf(name, sizeof name,
		"hlsl: DEPTH from TEXCOORD0's x, 0 to 1 across a quad, keeps its left half in front of a nearer quad at 0.5 "
		"and its right behind: %u of %u pixels wrong",
		(unsigned)w->depth, (unsigned)depth_pixels);
	check(name, landed && frames && w->depth == 0);
	(void)snprintf(name, sizeof name,
		"hlsl: a vertex shader's TEXCOORD3, COLOR1, TEXCOORD0 and FOG meet a pixel shader's COLOR1 and TEXCOORD3 by "
		"semantic, the other two unread: %u of %u pixels wrong",
		(unsigned)w->gaps, (unsigned)pixels);
	check(name, landed && frames && w->gaps == 0);
}

static void free_grid(Grid *g)
{
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(g->buffers[i]);
	xsdk_gpu_free(g->vertices);
	for (uint32_t i = 0; i < SHADERS; i++) {
		if (g->shaders[i].microcode)
			xsdk_shader_free(&g->shaders[i]);
		if (g->linked[i].microcode)
			xsdk_shader_free(&g->linked[i]);
	}
	// A shader's records point into its container (src/shader.c), so the containers go after.
	for (uint32_t i = 0; i < SHADERS; i++)
		xsdk_xsc_free(&g->compiled[i]);
}

static void run(Grid *g)
{
	Wrong wrong = { 0 };
	uint32_t logged = 0, checked = 0;
	bool landed = true, shown = true;
	for (uint32_t frame = 0; frame < FRAMES && landed; frame++) {
		volatile uint32_t *const buffer = g->buffers[frame % BUFFERS];
		landed = draw(g, buffer);
		// The first frame's colour and depth are whatever eDRAM held; its resolve clears them.
		if (frame > 0 && landed) {
			check_frame(buffer, frame, &wrong, &logged);
			checked++;
		}
		if (g->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(xsdk_gpu_address(buffer), WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("hlsl: VFACE's front quad read +1 %u, -1 %u and neither %u times, its back quad %u, %u and %u",
		(unsigned)wrong.faces[0][0], (unsigned)wrong.faces[0][1], (unsigned)wrong.faces[0][2],
		(unsigned)wrong.faces[1][0], (unsigned)wrong.faces[1][1], (unsigned)wrong.faces[1][2]);
	report(&wrong, landed, checked);
	if (g->has_display && shown) {
		xsdk_log_write("hlsl: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

void test_hlsl(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("hlsl: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0, height = 0;
	Grid g = { .has_display = NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH &&
			height == HEIGHT };
	const bool made = make_grid(&g);
	check("hlsl: xost compiles the grid's HLSL shaders on the console, and the grid is made", made);
	if (made) {
		char name[160];
		const uint32_t count = g.shaders[SHADER_GAPS].interpolator_count;
		(void)snprintf(name, sizeof name,
			"hlsl: the pixel shader reading COLOR1 and TEXCOORD3 declares %u interpolators in its container, not 14",
			(unsigned)count);
		check(name, count == GAPS_INTERPOLATORS);
		run(&g);
	}
	xsdk_display_close();
	free_grid(&g);
	check("hlsl: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
