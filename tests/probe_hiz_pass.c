#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Milestone 49's probe (docs/scope/49-hiz-clear-pass.md, step 1) of cheaper ways than the pass over the whole target to
// put the hierarchical buffer right after a depth clear. One case a launch, from a fresh boot, as a hang leaves the GPU
// hung: game:\hizpass_start.txt names it; without it the probe lists the cases and runs xsdk's pass. Each case is
// checked with milestone 30's draws after a clear (results/2026-10-13-milestone-30/probe_hiz_discovery7.inc) and
// xsdk_test's hierarchical depth checks (tests/test_hiz.c), and resolves clearing colour and depth are timed on the
// GPU's clock against resolves clearing colour alone. Built with src/gpu_resolve.c's hook (Makefile, hiz_pass_probes).

// The hook XSDK_HIZ_PASS_PROBE builds into src/gpu_resolve.c.
void xsdk_probe_hiz_pass(bool on, uint32_t event, const uint32_t rect[4]);

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 2000,
	CLEAR_ARGB = 0xFF202020,
	PROBE_ARGB = 0xFFE0E040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc), and RB_HIZCONTROL's test bit (docs/HARDWARE.md,
	// "Hierarchical depth, milestone 30").
	RB_HIZCONTROL = 0x2203,
	HIZ_TEST = 2,
	// Milestone 30's small rectangle, whose first block went wrong too.
	SMALL_X = 400,
	SMALL_Y = 300,
	SMALL_SIZE = 200,
	// A full-screen draw's first block, which went wrong without the pass.
	BLOCK_WIDTH = 32,
	BLOCK_HEIGHT = 8,
	TIMED_RESOLVES = 16,
	// The GPU's clock counts 498.75 a microsecond (docs/HARDWARE.md, "Queries and events").
	CLOCKS_IN_4_US = 1995,
	NAME_BYTES = 16,
	// Rectangles each first drawn after a clear, scattered, as the pass's place decided which draws it put right.
	SCATTERED = 64,
	SCATTERED_MOST_WIDTH = 400,
	SCATTERED_MOST_HEIGHT = 300,
	SCATTERED_LOGGED = 8,
	TEXT_BYTES = 160,
};

typedef struct {
	const char *name;
	const char *what;
	bool on;
	uint32_t event;
	uint32_t rect[4];
} PassCase;

// Events by number (xenia:src/xenia/gpu/xenos.h:1541-1566). Not 9 or 21, which hung the GPU, nor 5, 6, 7 and 8, 15 or
// 22, which changed nothing (docs/HARDWARE.md, "Hierarchical depth, milestone 30" and "Queries and events").
static const PassCase cases[] = {
	{ "full", "the pass over the whole target, as xsdk draws it", true, 0, { 0, 0, 0, 0 } },
	{ "none", "no pass, the control, which must leave pixels wrong", false, 0, { 0, 0, 0, 0 } },
	{ "rows8", "the pass over the top 8 rows", true, 0, { 0, 0, WIDTH, 8 } },
	{ "rows16", "the pass over the top 16 rows", true, 0, { 0, 0, WIDTH, 16 } },
	{ "rows32", "the pass over the top 32 rows", true, 0, { 0, 0, WIDTH, 32 } },
	{ "corner", "the pass over the top right 32x8 pixels", true, 0,
		{ WIDTH - BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "quarter", "the pass over the top left quarter, which holds the small rectangle's first block", true, 0,
		{ 0, 0, WIDTH / 2, HEIGHT / 2 } },
	// Whether the pass must touch some number of blocks, as rows8 put every pixel right and corner none.
	{ "strip2", "the pass over the top left 2 blocks, 64x8", true, 0, { 0, 0, 2 * BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "strip4", "the pass over the top left 4 blocks, 128x8", true, 0, { 0, 0, 4 * BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "strip8", "the pass over the top left 8 blocks, 256x8", true, 0, { 0, 0, 8 * BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "strip20", "the pass over the top left 20 blocks, 640x8", true, 0, { 0, 0, 20 * BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "column", "the pass over the left column of blocks, 32x720", true, 0, { 0, 0, BLOCK_WIDTH, HEIGHT } },
	// Whether one block is too few, or the corner fails as it's the block the next draw goes wrong in.
	{ "left1", "the pass over the top left block, 32x8", true, 0, { 0, 0, BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "corner2", "the pass over the top right 2 blocks, 64x8", true, 0,
		{ WIDTH - 2 * BLOCK_WIDTH, 0, 2 * BLOCK_WIDTH, BLOCK_HEIGHT } },
	{ "event4", "EVENT_WRITE CACHE_FLUSH_TS (4) in place of the pass", true, 4, { 0, 0, 0, 0 } },
	{ "event10", "EVENT_WRITE MPASS_PS_CP_REFETCH (10) in place of the pass", true, 10, { 0, 0, 0, 0 } },
	{ "event20", "EVENT_WRITE CACHE_FLUSH_AND_INV_TS_EVENT (20) in place of the pass", true, 20, { 0, 0, 0, 0 } },
};

enum { CASES = sizeof cases / sizeof cases[0] };

static const char *const start_path = "game:\\hizpass_start.txt";

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static int passed_count;
static int failed_count;
static bool stopped;
static XsdkShader vertex_shader;
static XsdkShader pixel_shader;
static XsdkDepthStencilState less_state;
static uint32_t vertices_address;
static volatile uint32_t *image;
static volatile uint32_t *clocks;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

// The case game:\hizpass_start.txt names; without it, or naming none, the cases are listed and xsdk's pass runs.
static const PassCase *chosen_case(void)
{
	FILE *const file = fopen(start_path, "r");
	char name[NAME_BYTES] = "";
	const bool read = file && fscanf(file, "%15s", name) == 1;
	if (file)
		(void)fclose(file);
	for (uint32_t i = 0; read && i < CASES; i++) {
		if (!strcmp(name, cases[i].name))
			return &cases[i];
	}
	xsdk_log_write("hiz pass probe: %s names no case; write one of these in it, one a boot:", start_path);
	for (uint32_t i = 0; i < CASES; i++)
		xsdk_log_write("hiz pass probe:   %s: %s", cases[i].name, cases[i].what);
	return &cases[0];
}

static bool wait_gpu(const char *step)
{
	if (stopped)
		return false;
	if (NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
		return true;
	xsdk_log_write("hiz pass: the GPU didn't finish %s; stopping", step);
	stopped = true;
	return false;
}

// A resolve clearing colour, and depth and the buffer to depth, with the case after it.
static void clear(float depth)
{
	xsdk_gpu_set_depth_clear(depth, 0);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	(void)wait_gpu("a depth clear");
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// Milestone 30's draw: the unit square over the rectangle, edges half a pixel early, at depth, less-than without
// writing, testing the buffer whichever side the clear took.
static void draw_probe(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float depth)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
	xsdk_gpu_set_depth_stencil(&less_state);
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = RB_HIZCONTROL;
	packet[1] = HIZ_TEST;
	const float left = (x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (x + w - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (y + h - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, depth, 0.0f },
		{ unorm(PROBE_ARGB >> 16), unorm(PROBE_ARGB >> 8), unorm(PROBE_ARGB), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static bool drawn(uint32_t x, uint32_t y)
{
	const uint32_t stored = image[tiled_index(x, y, WIDTH)];
	const uint32_t want = __builtin_bswap32(PROBE_ARGB);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
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
	*found = (Wrong){ .left = WIDTH, .top = HEIGHT };
	if (stopped)
		return false;
	draw_probe(x0, y0, w, h, depth);
	xsdk_gpu_set_resolve_clears(true, false);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_resolve_clears(true, true);
	if (!wait_gpu(name))
		return false;
	for (uint32_t y = y0; y < y0 + h; y++) {
		for (uint32_t x = x0; x < x0 + w; x++) {
			if (drawn(x, y) == shown)
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
	const uint32_t wrong = found.wrong;
	xsdk_log_write("hiz pass: %s: %u of %u pixels wrong, in (%u, %u) to (%u, %u)", name, (unsigned)wrong,
		(unsigned)(w * h), (unsigned)found.left, (unsigned)found.top, (unsigned)found.right, (unsigned)found.bottom);
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "hiz pass: %s, every pixel right", name);
	check(text, wrong == 0);
}

// Milestone 30's sequences after a clear: a draw testing a clear to 0.25 loads the buffer's old value, which without
// the pass hid 199 pixels of a full-screen draw after the next clear and 22 of the small one's.
static void check_after_clears(void)
{
	clear(1.0f);
	clear(0.25f);
	check_draw("cleared to 0.25 from far, a full-screen draw at 0.4 hidden", 0, 0, WIDTH, HEIGHT, 0.4f, false);
	clear(1.0f);
	check_draw("cleared far from 0.25, a full-screen draw at 0.9 shown", 0, 0, WIDTH, HEIGHT, 0.9f, true);
	clear(1.0f);
	clear(0.25f);
	check_draw("cleared to 0.25 from far, a 200x200 draw at 0.4 hidden", SMALL_X, SMALL_Y, SMALL_SIZE, SMALL_SIZE, 0.4f,
		false);
	clear(1.0f);
	check_draw(
		"cleared far from 0.25, a 200x200 draw at 0.9 shown", SMALL_X, SMALL_Y, SMALL_SIZE, SMALL_SIZE, 0.9f, true);
}

// Milestone 30's small sequence at the rectangle: a draw at 0.4 over a clear to 0.25 from far, then one at 0.9 over a
// clear to far, whose wrong pixels it counts.
static bool wrong_after_clears(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Wrong *shown)
{
	Wrong hidden;
	clear(1.0f);
	clear(0.25f);
	(void)count_wrong("a draw at 0.4", x, y, w, h, 0.4f, false, &hidden);
	clear(1.0f);
	return count_wrong("a draw at 0.9", x, y, w, h, 0.9f, true, shown);
}

// Rectangles whose first block, their top right, lies in the top row of blocks, where the small passes are drawn: the
// pass over the corner block left the draws starting in it wrong.
static const uint32_t top_row[][4] = {
	{ 0, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 2 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 19 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 20 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ WIDTH - 2 * BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ WIDTH - BLOCK_WIDTH, 0, BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 0, 0, 2 * BLOCK_WIDTH, BLOCK_HEIGHT },
	{ 0, 0, WIDTH / 2, BLOCK_HEIGHT },
	{ 0, 0, 2 * BLOCK_WIDTH, 100 },
	{ 0, 0, 1, 1 },
	{ 2 * BLOCK_WIDTH - 1, BLOCK_HEIGHT - 1, 1, 1 },
	{ WIDTH - 1, 0, 1, 1 },
	{ WIDTH - 1, BLOCK_HEIGHT - 1, 1, 1 },
	{ 0, BLOCK_HEIGHT, WIDTH, BLOCK_HEIGHT },
	{ WIDTH - BLOCK_WIDTH, BLOCK_HEIGHT, BLOCK_WIDTH, BLOCK_HEIGHT },
};

static void check_top_row(void)
{
	uint32_t wrong_rects = 0;
	for (uint32_t i = 0; i < sizeof top_row / sizeof top_row[0] && !stopped; i++) {
		const uint32_t *const r = top_row[i];
		Wrong shown;
		if (!wrong_after_clears(r[0], r[1], r[2], r[3], &shown))
			break;
		if (!shown.wrong)
			continue;
		wrong_rects++;
		xsdk_log_write("hiz pass: top row %ux%u at (%u, %u): %u pixels wrong, in (%u, %u) to (%u, %u)", (unsigned)r[2],
			(unsigned)r[3], (unsigned)r[0], (unsigned)r[1], (unsigned)shown.wrong, (unsigned)shown.left,
			(unsigned)shown.top, (unsigned)shown.right, (unsigned)shown.bottom);
	}
	xsdk_log_write("hiz pass: %u rectangles starting in the top rows: %u with wrong pixels",
		(unsigned)(sizeof top_row / sizeof top_row[0]), (unsigned)wrong_rects);
	check("hiz pass: rectangles starting in the top rows after clears, every pixel right", !stopped && !wrong_rects);
}

// Milestone 30's small sequence at rectangles of every size and place, from a fixed seed so every case draws the same.
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
		const uint32_t x = (seed >> 8) % (WIDTH - w + 1);
		seed = seed * 1664525 + 1013904223;
		const uint32_t y = (seed >> 8) % (HEIGHT - h + 1);
		Wrong shown;
		if (!wrong_after_clears(x, y, w, h, &shown))
			break;
		if (!shown.wrong)
			continue;
		if (wrong_rects++ < SCATTERED_LOGGED)
			xsdk_log_write("hiz pass: scattered %ux%u at (%u, %u): %u pixels wrong, in (%u, %u) to (%u, %u)",
				(unsigned)w, (unsigned)h, (unsigned)x, (unsigned)y, (unsigned)shown.wrong, (unsigned)shown.left,
				(unsigned)shown.top, (unsigned)shown.right, (unsigned)shown.bottom);
		wrong_pixels += shown.wrong;
	}
	xsdk_log_write("hiz pass: %u scattered rectangles: %u with wrong pixels, %u pixels in all", (unsigned)SCATTERED,
		(unsigned)wrong_rects, (unsigned)wrong_pixels);
	check("hiz pass: rectangles scattered after clears, every pixel right", !stopped && wrong_rects == 0);
}

// Hundredths of a microsecond a resolve.
static uint32_t centi_us_each(uint64_t taken)
{
	return (uint32_t)(taken * 400 / CLOCKS_IN_4_US / TIMED_RESOLVES);
}

// Resolves clearing colour, then colour and depth, each run between two copies of the GPU's clock, as test_gpu.c times
// a resolve; the difference is what a depth clear and the case after it cost.
static void time_resolves(void)
{
	uint64_t taken[2] = { 0, 0 };
	for (uint32_t depth = 0; depth < 2 && !stopped; depth++) {
		xsdk_gpu_set_resolve_clears(true, depth);
		xsdk_gpu_write_clock(xsdk_gpu_address(clocks), true);
		for (uint32_t i = 0; i < TIMED_RESOLVES; i++) {
			xsdk_gpu_set_target(WIDTH, HEIGHT);
			xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
		}
		xsdk_gpu_write_clock(xsdk_gpu_address(clocks + XSDK_GPU_CLOCK_WORDS), true);
		if (wait_gpu("the timed resolves"))
			taken[depth] = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clocks);
	}
	xsdk_gpu_set_resolve_clears(true, true);
	if (stopped)
		return;
	const uint32_t colour = centi_us_each(taken[0]);
	const uint32_t both = centi_us_each(taken[1]);
	const uint32_t more = both > colour ? both - colour : colour - both;
	xsdk_log_write(
		"hiz pass: %u 1280x720 resolves clearing colour took %u clocks, %u.%02u us each; clearing colour and "
		"depth %u clocks, %u.%02u us each: %s%u.%02u us each for depth",
		(unsigned)TIMED_RESOLVES, (unsigned)taken[0], (unsigned)(colour / 100), (unsigned)(colour % 100),
		(unsigned)taken[1], (unsigned)(both / 100), (unsigned)(both % 100), both < colour ? "-" : "",
		(unsigned)(more / 100), (unsigned)(more % 100));
}

static bool set_up(void)
{
	const XsdkDepthStencil less = { .depth_test = true, .depth_write = false, .depth_compare = XSDK_COMPARE_LESS };
	xsdk_gpu_depth_stencil_state(&less_state, &less);
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	clocks = xsdk_gpu_alloc(2 * XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t));
	if (!vertices || !image || !clocks ||
		!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	vertices_address = xsdk_gpu_address(vertices);
	xsdk_gpu_invalidate(vertices_address, sizeof square);
	return true;
}

int main(void)
{
	const PassCase *const c = chosen_case();
	xsdk_probe_hiz_pass(c->on, c->event, c->rect);
	xsdk_log_write("hiz pass probe: case %s, %s", c->name, c->what);
	const NTSTATUS status = xsdk_gpu_open();
	check("hiz pass: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return 1;
	const bool made = set_up();
	check("hiz pass: shaders and buffers", made);
	if (made) {
		xsdk_log_write("hiz pass: the first depth clear with the case, where a hang stops the log");
		check_after_clears();
		check_scattered();
		check_top_row();
		time_resolves();
	}
	check("hiz pass: GPU close", NT_SUCCESS(xsdk_gpu_close()));
	// The GPU is kept, hung, so xsdk_test's checks can't open it again.
	if (!stopped)
		test_hiz();
	xsdk_log_write("hiz pass probe: %s: %d checks passed, %d failed", c->name, passed_count, failed_count);
	return failed_count;
}
