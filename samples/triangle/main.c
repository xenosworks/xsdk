// triangle: GLSL shaders built by xsdk_add_shaders, vertices in GPU memory, a draw, a resolve and a present. The
// triangle's corners are red, green and blue, and the colours move round a corner once a second. It runs until Back,
// or until the title is left through the Guide's Xbox Home.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

enum {
	SPLASH_MS = 2000,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	// The Guide's first opening held a frame up 2.7 s (docs/HARDWARE.md, "The Guide, milestone 37").
	BEGIN_TIMEOUT_MS = 5000,
	FENCE_TIMEOUT_MS = 1000,
	TURN_FRAMES = 60,
	CLEAR_ARGB = 0xFF202020,
	CORNERS = 3,
	// x and y, then red, green and blue: triangle.vert's a_position and a_color.
	VERTEX_FLOATS = 5,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 8,
	VERTICES_BYTES = CORNERS * STRIDE_BYTES,
	MAX_WIDTH = 1280,
};

// Where each corner sits, -1 to 1 from the left of the screen to the right and from its bottom to its top.
static const float corner_places[CORNERS][2] = { { 0.0f, 0.75f }, { -0.75f, -0.75f }, { 0.75f, -0.75f } };
static const float corner_colours[CORNERS][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

// The vertex shader's inputs, by location: 0 is a position, 1 a colour (xenos-toolchain/docs/xsc.md, "Interface
// conventions").
static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = COLOR_OFFSET_BYTES },
};

static void check_frame(uint32_t address, uint32_t width, uint32_t height, uint32_t turn, const char *name);
static void log_checks(void);

// Corner i takes colour (i + turn) % 3.
static void write_vertices(volatile uint32_t *vertices, uint32_t turn)
{
	float corners[CORNERS][VERTEX_FLOATS];
	for (uint32_t i = 0; i < CORNERS; i++) {
		const float *const colour = corner_colours[(i + turn) % CORNERS];
		const float corner[VERTEX_FLOATS] = { corner_places[i][0], corner_places[i][1], colour[0], colour[1],
			colour[2] };
		memcpy(corners[i], corner, sizeof corner);
	}
	uint32_t words[VERTICES_BYTES / 4];
	memcpy(words, corners, sizeof corners);
	// GPU memory takes volatile 32-bit stores (docs/PITFALLS.md).
	for (uint32_t i = 0; i < VERTICES_BYTES / 4; i++)
		vertices[i] = words[i];
#ifdef SAMPLE_BREAK
	// Broken on purpose: the GPU's vertex cache keeps the colours it read before.
#else
	// The GPU's caches may hold what the memory held before.
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), VERTICES_BYTES);
#endif
}

static void draw(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices, uint32_t address, uint32_t width,
	uint32_t height)
{
	xsdk_gpu_set_target(width, height);
	xsdk_gpu_set_shaders(vertex, pixel);
	xsdk_built_shader_set_literals(&triangle_vert);
	xsdk_built_shader_set_literals(&triangle_frag);
	xsdk_gpu_set_vertices(0, vertices, VERTICES_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, CORNERS);
	// Copies the frame into the front buffer, then clears eDRAM for the next one.
	xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
}

static void run(
	const XsdkShader *vertex, const XsdkShader *pixel, volatile uint32_t *vertices, uint32_t width, uint32_t height)
{
	uint32_t turn = 0;
	write_vertices(vertices, turn);
	for (uint32_t frame = 0;; frame++) {
		INPUT_STATE input = { 0 };
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			break;
		if (frame % TURN_FRAMES == 0 && frame > 0) {
			// The GPU mustn't read the vertices while they change: wait until it has drawn every frame queued.
			const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
			if (!NT_SUCCESS(status))
				xsdk_log_write("triangle: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
			write_vertices(vertices, ++turn);
		}
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("triangle: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return;
		}
		draw(vertex, pixel, xsdk_gpu_address(vertices), address, width, height);
		// Frame 0's background is whatever eDRAM held, so the checks start at frame 1.
		if (frame == 1)
			check_frame(address, width, height, turn, "triangle: the first frame shows red, green and blue corners");
		if (frame == TURN_FRAMES)
			check_frame(
				address, width, height, turn, "triangle: the first frame after the colours moved shows them moved");
		xsdk_present_end();
	}
}

int main(void)
{
	xsdk_log_write("triangle: a triangle whose colours move round once a second; press Back to end");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("triangle: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("triangle: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	status = xsdk_shader_load(&vertex, triangle_vert.container, triangle_vert.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&pixel, triangle_frag.container, triangle_frag.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES });
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("triangle: loading the shaders failed: 0x%08X", (unsigned)status);
		return 1;
	}
	volatile uint32_t *const vertices = xsdk_gpu_alloc(VERTICES_BYTES);
	if (!vertices) {
		xsdk_log_write("triangle: xsdk_gpu_alloc failed");
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		// The title's exit frees GPU memory and gives the GPU back.
		xsdk_log_write("triangle: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	run(&vertex, &pixel, vertices, width, height);
	xsdk_present_close();
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("triangle: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	xsdk_log_write("triangle: done");
	log_checks();
	return 0;
}

// The checks: what the title can tell of itself from the log, as it can't see the screen.

enum {
	// Near a corner its own colour's channel is above this, and the other two below the low mark.
	CHANNEL_HIGH = 0xC0,
	CHANNEL_LOW = 0x40,
	OUTSIDE_X = 20,
	OUTSIDE_Y = 20,
};

static uint32_t passed_count;
static uint32_t failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static void log_checks(void)
{
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
}

// Pixel (x, y) of a frame the GPU has resolved, as 0xAARRGGBB. The frame lies in memory as a tiled texture does, so a
// download untiles it, and holds each pixel's bytes B, G, R, then A (docs/HARDWARE.md, "A title's buffer on screen").
static uint32_t frame_pixel(uint32_t address, uint32_t width, uint32_t height, uint32_t x, uint32_t y)
{
	XsdkTexture frame;
	if (width > MAX_WIDTH || !NT_SUCCESS(xsdk_texture_init(&frame, XSDK_FORMAT_A8R8G8B8, width, height, 1, true)))
		return 0;
	xsdk_texture_attach(&frame, xsdk_present_pixels(address));
	static uint32_t row[MAX_WIDTH];
	xsdk_texture_download_rows(&frame, 0, y, 1, row, sizeof row);
	return __builtin_bswap32(row[x]);
}

// A pixel 15% of the way from corner i to the middle, where nine tenths of the colour is the corner's.
static uint32_t corner_pixel(uint32_t address, uint32_t width, uint32_t height, uint32_t i)
{
	float middle[2] = { 0.0f, 0.0f };
	for (uint32_t c = 0; c < CORNERS; c++) {
		middle[0] += corner_places[c][0] / CORNERS;
		middle[1] += corner_places[c][1] / CORNERS;
	}
	const float x = corner_places[i][0] + 0.15f * (middle[0] - corner_places[i][0]);
	const float y = corner_places[i][1] + 0.15f * (middle[1] - corner_places[i][1]);
	return frame_pixel(
		address, width, height, (uint32_t)((x + 1.0f) / 2.0f * width), (uint32_t)((1.0f - y) / 2.0f * height));
}

static bool corner_holds(uint32_t pixel, const float *colour)
{
	for (uint32_t c = 0; c < 3; c++) {
		const uint32_t channel = pixel >> (16 - 8 * c) & 0xFF;
		if (colour[c] > 0.5f ? channel < CHANNEL_HIGH : channel > CHANNEL_LOW)
			return false;
	}
	return true;
}

// Once the GPU has finished the frame, each corner holds its colour for this turn and the background the clear colour.
static void check_frame(uint32_t address, uint32_t width, uint32_t height, uint32_t turn, const char *name)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("triangle: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
	const uint32_t outside = frame_pixel(address, width, height, OUTSIDE_X, OUTSIDE_Y);
	bool held = NT_SUCCESS(status) && outside == CLEAR_ARGB;
	if (outside != CLEAR_ARGB)
		xsdk_log_write("triangle: pixel (%u, %u) held %08X, not the clear colour %08X", (unsigned)OUTSIDE_X,
			(unsigned)OUTSIDE_Y, (unsigned)outside, (unsigned)CLEAR_ARGB);
	for (uint32_t i = 0; i < CORNERS; i++) {
		const uint32_t pixel = corner_pixel(address, width, height, i);
		if (!corner_holds(pixel, corner_colours[(i + turn) % CORNERS])) {
			xsdk_log_write("triangle: turn %u: corner %u held %08X", (unsigned)turn, (unsigned)i, (unsigned)pixel);
			held = false;
		}
	}
	check(name, held);
}
