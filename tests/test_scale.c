#include <stdbool.h>
#include <stdint.h>

#include "probe_gpu.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

// Shows a 1280x720 image on the SDK's scaling long enough for a capture, and checks the scaler's settings, then
// letterboxing's, forced, through the present.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	// A few frames, so the scaler has taken locked settings at a vblank before they're read.
	SETTLE_MS = 100,
	CORNER = 32,
	BLOCK = 128,
	// Blue-grey, a tint no other test shows, so a capture shows this test.
	BACKGROUND = 0xFF305070,
	WHITE = 0xFFFFFFFF,
	BLACK = 0xFF000000,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	SCALE_ONE_LOG2 = 24,
	// The scaler keeps the scale factors to 0x20 and the phases to 0x200 (docs/HARDWARE.md).
	SCALE_KEPT = ~0x1Fu,
	PHASE_KEPT = ~0x1FFu,
	TWO_TAPS = 0x101,
	BUILT_IN_FILTER = 0x100,
	// Letterboxed, a front buffer is 1280x960 with the frame from row 128 (src/present.c).
	LETTERBOX_HEIGHT = 960,
	LETTERBOX_TOP = 128,
	LETTERBOX_BUFFERS = 2,
	LETTERBOX_FRAMES = 10,
	BEGIN_TIMEOUT_MS = 1000,
	// Opaque black as present fills its buffers, and the columns read on each row.
	STORED_BLACK = 0x000000FF,
	COLUMNS = 3,
	MAX_WRONG_LOGGED = 4,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	AVIVO_D1MODE_DESKTOP_HEIGHT = 0x194B,
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	UNKNOWN_196C = 0x196C,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_1970 = 0x1970,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	UNKNOWN_197A = 0x197A,
};

enum {
	SCALER_ENABLE,
	SCALER_TAP_CONTROL,
	VIEWPORT_SIZE,
	VIEWPORT_WIDTH_QUARTER,
	HORIZONTAL_FILTER,
	HORIZONTAL_SCALE,
	HORIZONTAL_PHASE,
	VERTICAL_FILTER,
	VERTICAL_SCALE,
	VERTICAL_PHASE,
	SCALER_REGISTERS,
};

static const uint32_t scaler_registers[SCALER_REGISTERS] = { AVIVO_D1SCL_SCALER_ENABLE, AVIVO_D1SCL_SCALER_TAP_CONTROL,
	AVIVO_D1MODE_VIEWPORT_SIZE, UNKNOWN_197A, UNKNOWN_196C, UNKNOWN_196D, UNKNOWN_196E, UNKNOWN_1970, UNKNOWN_1971,
	UNKNOWN_1972 };

// The display reads a word's bytes as B, G, R, A (docs/HARDWARE.md, "A title's buffer on screen").
static void put(volatile uint32_t *pixels, uint32_t x, uint32_t y, uint32_t argb)
{
	pixels[tiled_index(x, y, WIDTH)] = __builtin_bswap32(argb);
}

static void fill(volatile uint32_t *pixels, uint32_t x0, uint32_t y0, uint32_t size, uint32_t argb)
{
	for (uint32_t y = y0; y < y0 + size; y++)
		for (uint32_t x = x0; x < x0 + size; x++)
			put(pixels, x, y, argb);
}

// A one-pixel white border, a square in each corner, and blocks of one-pixel stripes and checks in the middle.
static void draw_image(volatile uint32_t *pixels)
{
	for (uint32_t y = 0; y < HEIGHT; y++)
		for (uint32_t x = 0; x < WIDTH; x++)
			put(pixels, x, y, x == 0 || y == 0 || x == WIDTH - 1 || y == HEIGHT - 1 ? WHITE : BACKGROUND);
	fill(pixels, 1, 1, CORNER, RED);
	fill(pixels, WIDTH - 1 - CORNER, 1, CORNER, GREEN);
	fill(pixels, 1, HEIGHT - 1 - CORNER, CORNER, BLUE);
	fill(pixels, WIDTH - 1 - CORNER, HEIGHT - 1 - CORNER, CORNER, YELLOW);
	const uint32_t top = HEIGHT / 2 - BLOCK / 2;
	for (uint32_t y = 0; y < BLOCK; y++) {
		for (uint32_t x = 0; x < BLOCK; x++) {
			put(pixels, WIDTH / 2 - BLOCK * 3 / 2 + x, top + y, x & 1 ? WHITE : BLACK);
			put(pixels, WIDTH / 2 - BLOCK / 2 + x, top + y, y & 1 ? WHITE : BLACK);
			put(pixels, WIDTH / 2 + BLOCK / 2 + x, top + y, (x ^ y) & 1 ? WHITE : BLACK);
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

static void read_scaler(uint32_t *values)
{
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++)
		values[i] = probe_gpu_registers[scaler_registers[i]];
}

// What the scaler should read back for a 1280-wide surface this high on this output, worked out apart from
// src/display.c.
static void expect_scaler(const VIDEO_MODE *video, uint32_t height, const uint32_t *start, uint32_t *expected)
{
	const uint32_t horizontal = (uint32_t)(((uint64_t)WIDTH << SCALE_ONE_LOG2) / video->display_width);
	const uint32_t vertical = (uint32_t)(((uint64_t)height << SCALE_ONE_LOG2) / video->display_height);
	expected[SCALER_ENABLE] = video->display_width != WIDTH || video->display_height != height;
	expected[SCALER_TAP_CONTROL] = TWO_TAPS;
	expected[VIEWPORT_SIZE] = WIDTH << 16 | height;
	expected[VIEWPORT_WIDTH_QUARTER] = WIDTH / 4 - 1;
	expected[HORIZONTAL_FILTER] = start[HORIZONTAL_FILTER] | BUILT_IN_FILTER;
	expected[HORIZONTAL_SCALE] = horizontal & SCALE_KEPT;
	// 2 taps: (scale + 3) / 2 (docs/HARDWARE.md, "What AMD's code says about the scaler").
	expected[HORIZONTAL_PHASE] = ((3u << 15) + (horizontal >> 9)) & PHASE_KEPT;
	expected[VERTICAL_FILTER] = start[VERTICAL_FILTER] | BUILT_IN_FILTER;
	expected[VERTICAL_SCALE] = vertical & SCALE_KEPT;
	expected[VERTICAL_PHASE] = ((3u << 15) + (vertical >> 9)) & PHASE_KEPT;
}

// Compares each register, logging the ones that differ when asked to.
static bool scaler_matches(const uint32_t *values, const uint32_t *expected, const char *when, bool log)
{
	bool matches = true;
	for (uint32_t i = 0; i < SCALER_REGISTERS; i++) {
		if (values[i] == expected[i])
			continue;
		if (log)
			xsdk_log_write("scale: %s: register %04X held %08X, not %08X", when, (unsigned)scaler_registers[i],
				(unsigned)values[i], (unsigned)expected[i]);
		matches = false;
	}
	return matches;
}

// A title asking for the kernel's mode gets its viewport's size, and the scaler stays as the kernel set it.
static void check_kernel_mode(const uint32_t *start)
{
	uint32_t width = 0;
	uint32_t height = 0;
	const bool opened = NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_KERNEL, &width, &height));
	const bool kernel_size = opened && width == start[VIEWPORT_SIZE] >> 16 && height == (start[VIEWPORT_SIZE] & 0xFFFF);
	check("scale: the kernel's mode opens at the kernel's size", kernel_size);
	volatile uint32_t *const pixels = kernel_size ? xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(width, height)) : NULL;
	uint32_t values[SCALER_REGISTERS];
	if (pixels) {
		for (uint32_t i = 0; i < xsdk_gpu_resolve_bytes(width, height) / 4; i++)
			pixels[i] = __builtin_bswap32(BACKGROUND);
		__asm__ volatile("sync" ::: "memory");
		check("scale: kernel's mode: buffer shown", NT_SUCCESS(xsdk_display_show(xsdk_gpu_address(pixels), width)));
		(void)xsdk_thread_sleep(SETTLE_MS);
		read_scaler(values);
		check("scale: the kernel's mode leaves the scaler alone", scaler_matches(values, start, "kernel's mode", true));
	}
	xsdk_display_close();
	(void)xsdk_thread_sleep(SETTLE_MS);
	read_scaler(values);
	check("scale: the kernel's scaler settings are still there", scaler_matches(values, start, "closed again", true));
	xsdk_gpu_free(pixels);
}

// The front buffer on screen, from its start: black above the frame and below it, the frame's colour between, at the
// left edge, the middle and the right edge of every row.
static bool letterbox_holds(uint32_t frame_address)
{
	const uint32_t shown = xsdk_display_shown_address();
	if (shown > frame_address)
		return false;
	volatile const uint32_t *const buffer = xsdk_present_pixels(frame_address) - (frame_address - shown) / 4;
	const uint32_t columns[COLUMNS] = { 0, WIDTH / 2, WIDTH - 1 };
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < LETTERBOX_HEIGHT; y++) {
		const bool in_frame = y >= LETTERBOX_TOP && y < LETTERBOX_TOP + HEIGHT;
		const uint32_t expected = in_frame ? __builtin_bswap32(BACKGROUND) : STORED_BLACK;
		for (uint32_t i = 0; i < COLUMNS; i++) {
			const uint32_t held = buffer[tiled_index(columns[i], y, WIDTH)];
			if (held == expected)
				continue;
			if (wrong < MAX_WRONG_LOGGED)
				xsdk_log_write("scale: letterboxed: row %u, column %u held %08X, not %08X", (unsigned)y,
					(unsigned)columns[i], (unsigned)held, (unsigned)expected);
			wrong++;
		}
	}
	return wrong == 0;
}

// Letterboxing forced, as on a 4:3 screen, through the present as a title shows its frames: the display shows the
// 1280x960 front buffer with the scaler planned for it, the frame resolved between black bars, and the scaler goes
// back as the present closes.
static void check_letterbox(const VIDEO_MODE *video, const uint32_t *start)
{
	xsdk_display_force_letterbox(true);
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS status = xsdk_present_open(XSDK_DISPLAY_720P, LETTERBOX_BUFFERS, 1, &width, &height);
	xsdk_display_force_letterbox(false);
	uint32_t presented = 0;
	uint32_t address = 0;
	if (NT_SUCCESS(status) && width == WIDTH && height == HEIGHT) {
		for (; presented < LETTERBOX_FRAMES && NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS));
			presented++) {
			// Each frame holds the colour the resolve before it cleared to.
			xsdk_gpu_resolve(address, WIDTH, HEIGHT, BACKGROUND);
			xsdk_present_end();
		}
	}
	XsdkPresentStats stats;
	test_present_stats_at_last_flip(presented, &stats);
	const bool shown = presented == LETTERBOX_FRAMES && stats.flips == LETTERBOX_FRAMES;
	uint32_t values[SCALER_REGISTERS];
	read_scaler(values);
	uint32_t expected[SCALER_REGISTERS];
	expect_scaler(video, LETTERBOX_HEIGHT, start, expected);
	const bool surface = probe_gpu_registers[D1GRPH_PITCH] == WIDTH && probe_gpu_registers[D1GRPH_X_END] == WIDTH &&
		probe_gpu_registers[D1GRPH_Y_END] == LETTERBOX_HEIGHT;
	if (shown && !surface)
		xsdk_log_write("scale: letterboxed: the surface's pitch, width and height read %u, %u and %u",
			(unsigned)probe_gpu_registers[D1GRPH_PITCH], (unsigned)probe_gpu_registers[D1GRPH_X_END],
			(unsigned)probe_gpu_registers[D1GRPH_Y_END]);
	check("scale: letterboxed, the display shows a 1280x960 front buffer, the scaler planned for it",
		shown && surface && scaler_matches(values, expected, "letterboxed", true));
	check("scale: letterboxed, the front buffer is black above row 128 and from row 848, the frame between",
		shown && letterbox_holds(address));
	if (NT_SUCCESS(status))
		xsdk_present_close();
	(void)xsdk_thread_sleep(SETTLE_MS);
	read_scaler(values);
	check("scale: letterboxing's scaler is taken back as the present closes",
		scaler_matches(values, start, "letterbox closed", true));
}

void test_scale(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("scale: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	VIDEO_MODE video;
	VdQueryVideoMode(&video);
	const bool has_output = video.display_width && video.display_height;
	check("scale: the kernel reports the output's size", has_output);
	if (!has_output) {
		(void)xsdk_gpu_close();
		return;
	}
	uint32_t start[SCALER_REGISTERS];
	read_scaler(start);
	uint32_t expected[SCALER_REGISTERS];
	expect_scaler(&video, HEIGHT, start, expected);
	// The kernel's own 640x480 scaling must fail the comparison, or it would pass anything.
	check("scale: the kernel's own scaler settings fail the check", !scaler_matches(start, expected, "", false));
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("scale: display open at 1280x720", has_display);
	volatile uint32_t *const pixels = has_display ? xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT)) : NULL;
	if (pixels) {
		draw_image(pixels);
		check("scale: image shown", NT_SUCCESS(xsdk_display_show(xsdk_gpu_address(pixels), WIDTH)));
		(void)xsdk_thread_sleep(SETTLE_MS);
		uint32_t values[SCALER_REGISTERS];
		read_scaler(values);
		check("scale: the scaler holds 2 taps, the built-in filter and the output's scale factors",
			scaler_matches(values, expected, "shown", true));
		xsdk_log_write("scale: 1280x720 on the %ux%u output, desktop height %u; showing it for %u ms for a capture",
			(unsigned)video.display_width, (unsigned)video.display_height,
			(unsigned)probe_gpu_registers[AVIVO_D1MODE_DESKTOP_HEIGHT], (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
	xsdk_display_close();
	if (has_display) {
		(void)xsdk_thread_sleep(SETTLE_MS);
		uint32_t values[SCALER_REGISTERS];
		read_scaler(values);
		check("scale: the kernel's scaler settings are back", scaler_matches(values, start, "closed", true));
	}
	xsdk_gpu_free(pixels);
	if (has_display) {
		check_kernel_mode(start);
		check_letterbox(&video, start);
	}
	check("scale: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
