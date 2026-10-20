#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 54 step 5's probe (docs/scope/54-porters-files-and-system.md): which display registers place the scaled
// picture in the output, to letterbox 1280x720 on a 4:3 screen. game:\letterbox_case.txt names the case; without it,
// "read" runs. "read" logs the display controller's registers, VdQueryVideoMode and VdGetCurrentDisplayInformation,
// writing nothing, and ends. The other cases take the display as xsdk does and show a test picture: a white border,
// 160-pixel squares from the centre, a ring of radius 300 there, and red, green, blue and yellow corners at the top
// left, top right, bottom left and bottom right. Square squares and a round ring mean unstretched; the border and four
// corners in view mean nothing is cut off. "fill" shows 1280x720 as xsdk does today, filling the output. "letterbox"
// shows it across the output's width at 3/4 of its height, as 16:9 sits on a 4:3 screen, bars above and below;
// "pillar" a 960x720 picture at the output's height and 3/4 of its width, as 4:3 sits on a 16:9 screen, bars left and
// right, whatever screen VdQueryVideoMode reports. Both place it as radeonhd does for scaling that keeps the aspect,
// with AVIVO's external overscan as the bars and 0x659C at 1 (xf86-video-radeonhd:src/rhd_crtc.c:612-679, 721-790),
// and scale as xsdk does into what's left; each register written is logged, then read back. A case that shows the
// picture runs until Back on controller 1, which puts the overscan and 0x659C back, or until the console agent ends
// the title, which leaves them as written; "read" shows what the next title finds.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PILLAR_WIDTH = 960,
	SQUARE = 160,
	// Keeps the squares' count from the centre positive.
	SQUARE_BIAS = 10 * SQUARE,
	RADIUS = 300,
	RING_HALF_WIDTH = 2,
	CORNER = 64,
	WHITE = 0xFFFFFFFF,
	BLACK = 0xFF000000,
	DARK = 0xFF303030,
	LIGHT = 0xFF606060,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	CYAN = 0xFF00FFFF,
	SCALE_ONE_LOG2 = 24,
	// xsdk's scaler settings (src/display.c): 2 taps each way through the built-in filter.
	TWO_TAPS = 0x101,
	TAPS = 2,
	BUILT_IN_FILTER = 0x100,
	// Holds the scaler's registers until it's released (xf86-video-radeonhd:src/rhd_regs.h:412-413).
	SCALER_UPDATE_LOCK = 0x10000,
	// Long enough for the display to take new values at a vblank.
	SETTLE_MS = 100,
	POLL_MS = 50,
	NAME_BYTES = 16,
};

// Display registers by index: libxenon's names (libxenon:drivers/xenos/xenos.h:11-60), else Xenia's style.
enum {
	D1CRTC_H_TOTAL = 0x1800,
	D1CRTC_H_BLANK_START_END = 0x1801,
	D1CRTC_V_TOTAL = 0x1808,
	AVIVO_D1CRTC_V_BLANK_START_END = 0x1809,
	// The kernel's scaler builder writes these four (docs/HARDWARE.md, "The kernel's scaler packets name the
	// registers but carry zeros"); libxenon writes them 0.
	D1CRTC_MVP_FIFO_STATUS = 0x1811,
	D1CRTC_MVP_SLAVE_STATUS = 0x1812,
	D1CRTC_MVP_INBAND_CNTL_CAP = 0x1813,
	D1CRTC_MVP_INBAND_CNTL_INSERT = 0x1814,
	// D1CRTC_OVERSCAN_COLOR's offset in AMD's M56 register guide (x.org/docs/AMD/old).
	UNKNOWN_1825 = 0x1825,
	D1GRPH_SURFACE_OFFSET_X = 0x1849,
	D1GRPH_SURFACE_OFFSET_Y = 0x184A,
	D1GRPH_X_START = 0x184B,
	D1GRPH_Y_START = 0x184C,
	AVIVO_D1MODE_DATA_FORMAT = 0x194A,
	AVIVO_D1MODE_DESKTOP_HEIGHT = 0x194B,
	// xenia:src/xenia/gpu/graphics_system.cc:189-192
	R500_D1MODE_V_COUNTER = 0x194C,
	UNKNOWN_195E = 0x195E,
	AVIVO_D1MODE_VIEWPORT_START = 0x1960,
	// radeonhd's D1MODE_EXT_OVERSCAN_LEFT_RIGHT and _TOP_BOTTOM, left and top in the high half
	// (xf86-video-radeonhd:src/rhd_regs.h:200-201, rhd_crtc.c:749-752).
	UNKNOWN_1962 = 0x1962,
	UNKNOWN_1963 = 0x1963,
	AVIVO_D1SCL_SCALER_TAP_CONTROL = 0x1965,
	UNKNOWN_1966 = 0x1966,
	// radeonhd's guess D1MODE_CENTER, 1 wherever it sets overscan (xf86-video-radeonhd:src/rhd_regs.h:207,
	// rhd_crtc.c:761-773).
	UNKNOWN_1967 = 0x1967,
	UNKNOWN_1968 = 0x1968,
	// radeonhd's guess D1SCL_HVSCALE (rhd_regs.h:208), AMD's later automatic scaling (docs/HARDWARE.md).
	UNKNOWN_1969 = 0x1969,
	// Filter control, scale factor and start phase, horizontal then vertical (docs/HARDWARE.md).
	UNKNOWN_196C = 0x196C,
	UNKNOWN_196D = 0x196D,
	UNKNOWN_196E = 0x196E,
	UNKNOWN_196F = 0x196F,
	UNKNOWN_1970 = 0x1970,
	UNKNOWN_1971 = 0x1971,
	UNKNOWN_1972 = 0x1972,
	AVIVO_D1SCL_UPDATE = 0x1973,
	UNKNOWN_1977 = 0x1977,
	// The viewport's width over 4, less one (libxenon:drivers/xenos/xenos.c:390, 397).
	UNKNOWN_197A = 0x197A,
};

typedef struct {
	uint32_t index;
	const char *name;
} Register;

// What xsdk's display code writes (src/display.c), the viewport, the desktop and the CRTC's active size, and what the
// kernel's scaler builder writes besides, all read only.
static const Register logged[] = {
	{ D1CRTC_H_TOTAL, "D1CRTC_H_TOTAL" },
	{ D1CRTC_H_BLANK_START_END, "D1CRTC_H_BLANK_START_END" },
	{ D1CRTC_V_TOTAL, "D1CRTC_V_TOTAL" },
	{ AVIVO_D1CRTC_V_BLANK_START_END, "AVIVO_D1CRTC_V_BLANK_START_END" },
	{ D1CRTC_MVP_FIFO_STATUS, "D1CRTC_MVP_FIFO_STATUS" },
	{ D1CRTC_MVP_SLAVE_STATUS, "D1CRTC_MVP_SLAVE_STATUS" },
	{ D1CRTC_MVP_INBAND_CNTL_CAP, "D1CRTC_MVP_INBAND_CNTL_CAP" },
	{ D1CRTC_MVP_INBAND_CNTL_INSERT, "D1CRTC_MVP_INBAND_CNTL_INSERT" },
	{ UNKNOWN_1825, "UNKNOWN_1825 (overscan colour)" },
	{ D1CRTC_DOUBLE_BUFFER_CONTROL, "D1CRTC_DOUBLE_BUFFER_CONTROL" },
	{ UNKNOWN_0A10, "UNKNOWN_0A10" },
	{ UNKNOWN_0A11, "UNKNOWN_0A11" },
	{ D1GRPH_ENABLE, "D1GRPH_ENABLE" },
	{ D1GRPH_CONTROL, "D1GRPH_CONTROL" },
	{ D1GRPH_PRIMARY_SURFACE_ADDRESS, "D1GRPH_PRIMARY_SURFACE_ADDRESS" },
	{ D1GRPH_PITCH, "D1GRPH_PITCH" },
	{ D1GRPH_SURFACE_OFFSET_X, "D1GRPH_SURFACE_OFFSET_X" },
	{ D1GRPH_SURFACE_OFFSET_Y, "D1GRPH_SURFACE_OFFSET_Y" },
	{ D1GRPH_X_START, "D1GRPH_X_START" },
	{ D1GRPH_Y_START, "D1GRPH_Y_START" },
	{ D1GRPH_X_END, "D1GRPH_X_END" },
	{ D1GRPH_Y_END, "D1GRPH_Y_END" },
	{ DC_LUT_RW_MODE, "DC_LUT_RW_MODE" },
	{ DC_LUT_RW_INDEX, "DC_LUT_RW_INDEX" },
	{ DC_LUT_WRITE_EN_MASK, "DC_LUT_WRITE_EN_MASK" },
	{ DC_LUT_AUTOFILL, "DC_LUT_AUTOFILL" },
	{ DC_LUTA_CONTROL, "DC_LUTA_CONTROL" },
	{ AVIVO_D1MODE_DATA_FORMAT, "AVIVO_D1MODE_DATA_FORMAT" },
	{ AVIVO_D1MODE_DESKTOP_HEIGHT, "AVIVO_D1MODE_DESKTOP_HEIGHT" },
	{ R500_D1MODE_V_COUNTER, "R500_D1MODE_V_COUNTER" },
	{ UNKNOWN_195E, "UNKNOWN_195E" },
	{ AVIVO_D1MODE_VIEWPORT_START, "AVIVO_D1MODE_VIEWPORT_START" },
	{ AVIVO_D1MODE_VIEWPORT_SIZE, "AVIVO_D1MODE_VIEWPORT_SIZE" },
	{ UNKNOWN_1962, "UNKNOWN_1962 (overscan left, right)" },
	{ UNKNOWN_1963, "UNKNOWN_1963 (overscan top, bottom)" },
	{ AVIVO_D1SCL_SCALER_ENABLE, "AVIVO_D1SCL_SCALER_ENABLE" },
	{ AVIVO_D1SCL_SCALER_TAP_CONTROL, "AVIVO_D1SCL_SCALER_TAP_CONTROL" },
	{ UNKNOWN_1966, "UNKNOWN_1966" },
	{ UNKNOWN_1967, "UNKNOWN_1967" },
	{ UNKNOWN_1968, "UNKNOWN_1968" },
	{ UNKNOWN_1969, "UNKNOWN_1969" },
	{ UNKNOWN_196C, "UNKNOWN_196C (horizontal filter)" },
	{ UNKNOWN_196D, "UNKNOWN_196D (horizontal scale)" },
	{ UNKNOWN_196E, "UNKNOWN_196E (horizontal phase)" },
	{ UNKNOWN_196F, "UNKNOWN_196F" },
	{ UNKNOWN_1970, "UNKNOWN_1970 (vertical filter)" },
	{ UNKNOWN_1971, "UNKNOWN_1971 (vertical scale)" },
	{ UNKNOWN_1972, "UNKNOWN_1972 (vertical phase)" },
	{ AVIVO_D1SCL_UPDATE, "AVIVO_D1SCL_UPDATE" },
	{ UNKNOWN_1977, "UNKNOWN_1977" },
	{ UNKNOWN_197A, "UNKNOWN_197A (viewport width / 4 - 1)" },
};

enum { LOGGED = sizeof logged / sizeof logged[0] };

// The registers a placed picture writes: xsdk's scaler set, in src/display.c's order, then what radeonhd adds to keep
// the aspect.
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
	OVERSCAN_LEFT_RIGHT,
	OVERSCAN_TOP_BOTTOM,
	CENTER,
	WRITTEN,
};

static const uint32_t written[WRITTEN] = { AVIVO_D1SCL_SCALER_ENABLE, AVIVO_D1SCL_SCALER_TAP_CONTROL,
	AVIVO_D1MODE_VIEWPORT_SIZE, UNKNOWN_197A, UNKNOWN_196C, UNKNOWN_196D, UNKNOWN_196E, UNKNOWN_1970, UNKNOWN_1971,
	UNKNOWN_1972, UNKNOWN_1962, UNKNOWN_1963, UNKNOWN_1967 };

// What xsdk's display close doesn't put back, so the probe does.
static const uint32_t restored[] = { UNKNOWN_1962, UNKNOWN_1963, UNKNOWN_1967 };

enum { RESTORED = sizeof restored / sizeof restored[0] };

typedef enum { FILL, LETTERBOX, PILLAR } Shape;

// The picture on the output, and the bars around it.
typedef struct {
	uint32_t source_width;
	uint32_t width;
	uint32_t height;
	uint32_t left;
	uint32_t right;
	uint32_t top;
	uint32_t bottom;
} Placement;

static const char *const case_path = "game:\\letterbox_case.txt";

static void log_registers(const char *when)
{
	for (uint32_t i = 0; i < LOGGED; i++) {
		const uint32_t index = logged[i].index;
		xsdk_log_write("letterbox: %s: %04X (byte %04X) %08X %s", when, (unsigned)index, (unsigned)(index * 4),
			(unsigned)probe_gpu_registers[index], logged[i].name);
	}
}

static void log_modes(void)
{
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	xsdk_log_write(
		"letterbox: VdQueryVideoMode: %ux%u, interlaced %u, widescreen %u, hi-def %u, %.3f Hz, standard %u, "
		"reserved %08X %08X %08X %08X %08X",
		(unsigned)mode.display_width, (unsigned)mode.display_height, (unsigned)mode.is_interlaced,
		(unsigned)mode.is_widescreen, (unsigned)mode.is_hi_def, (double)mode.refresh_rate,
		(unsigned)mode.video_standard, (unsigned)mode.reserved[0], (unsigned)mode.reserved[1],
		(unsigned)mode.reserved[2], (unsigned)mode.reserved[3], (unsigned)mode.reserved[4]);
	DISPLAY_INFO info;
	VdGetCurrentDisplayInformation(&info);
	xsdk_log_write(
		"letterbox: VdGetCurrentDisplayInformation: front buffer %ux%u, formats %u %u, display %ux%u "
		"(actual width %u), %.3f Hz, interlaced %u, colour format %u, window overscan %u %u %u %u",
		(unsigned)info.front_buffer_width, (unsigned)info.front_buffer_height, (unsigned)info.front_buffer_color_format,
		(unsigned)info.front_buffer_pixel_format, (unsigned)info.display_width, (unsigned)info.display_height,
		(unsigned)info.actual_display_width, (double)info.display_refresh_rate, (unsigned)info.display_interlaced,
		(unsigned)info.display_color_format, (unsigned)info.display_window_overscan[0],
		(unsigned)info.display_window_overscan[1], (unsigned)info.display_window_overscan[2],
		(unsigned)info.display_window_overscan[3]);
	for (uint32_t i = 0; i < sizeof info.scaler_parameters / sizeof info.scaler_parameters[0]; i++)
		xsdk_log_write("letterbox: VdGetCurrentDisplayInformation: scaler parameter %u: %08X", (unsigned)i,
			(unsigned)info.scaler_parameters[i]);
}

static uint32_t picture_argb(uint32_t x, uint32_t y, uint32_t width)
{
	const uint32_t right = width - 1;
	const uint32_t bottom = HEIGHT - 1;
	if (x == 0 || y == 0 || x == right || y == bottom)
		return WHITE;
	if (x <= CORNER && y <= CORNER)
		return RED;
	if (x >= right - CORNER && y <= CORNER)
		return GREEN;
	if (x <= CORNER && y >= bottom - CORNER)
		return BLUE;
	if (x >= right - CORNER && y >= bottom - CORNER)
		return YELLOW;
	const int32_t dx = (int32_t)x - (int32_t)(width / 2);
	const int32_t dy = (int32_t)y - HEIGHT / 2;
	const int32_t distance_squared = dx * dx + dy * dy;
	const int32_t inner = RADIUS - RING_HALF_WIDTH;
	const int32_t outer = RADIUS + RING_HALF_WIDTH;
	if (distance_squared >= inner * inner && distance_squared <= outer * outer)
		return CYAN;
	const uint32_t squares = (x + SQUARE_BIAS - width / 2) / SQUARE + (y + SQUARE_BIAS - HEIGHT / 2) / SQUARE;
	return squares & 1 ? LIGHT : DARK;
}

// The display reads a word's bytes as B, G, R, A (docs/HARDWARE.md, "A title's buffer on screen"); past the picture's
// width, black.
static void draw_picture(volatile uint32_t *pixels, uint32_t width)
{
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++)
			pixels[tiled_index(x, y, WIDTH)] = __builtin_bswap32(x < width ? picture_argb(x, y, width) : BLACK);
	}
	__asm__ volatile("sync" ::: "memory");
}

// Source over output in 8.24 (docs/HARDWARE.md, "Scaling 1280x720 by hand").
static uint32_t scale(uint32_t source, uint32_t output)
{
	return (uint32_t)(((uint64_t)source << SCALE_ONE_LOG2) / output);
}

// (scale + taps + 1) / 2 in 16.16 (docs/HARDWARE.md, "What AMD's code says about the scaler").
static uint32_t phase(uint32_t scale_8_24)
{
	return ((TAPS + 1) << 15) + (scale_8_24 >> 9);
}

// The picture as it sits on a 4:3 screen at 16:9, or on a 16:9 screen at 4:3, its size a multiple of 4 and the bars
// shared out evenly.
static Placement plan(const VIDEO_MODE *mode, Shape shape)
{
	const uint32_t output_width = mode->display_width;
	const uint32_t output_height = mode->display_height;
	Placement placement = {
		.source_width = shape == PILLAR ? PILLAR_WIDTH : WIDTH,
		.width = shape == PILLAR ? (output_width * 3 / 4) & ~3u : output_width,
		.height = shape == LETTERBOX ? (output_height * 3 / 4) & ~3u : output_height,
	};
	placement.left = (output_width - placement.width) / 2;
	placement.right = output_width - placement.width - placement.left;
	placement.top = (output_height - placement.height) / 2;
	placement.bottom = output_height - placement.height - placement.top;
	return placement;
}

// xsdk's scaling (src/display.c, plan_values) into the placement's picture, and the bars as overscan.
static void plan_values(const Placement *placement, uint32_t *values)
{
	const uint32_t horizontal = scale(placement->source_width, placement->width);
	const uint32_t vertical = scale(HEIGHT, placement->height);
	values[SCALER_ENABLE] = 1;
	values[SCALER_TAP_CONTROL] = TWO_TAPS;
	values[VIEWPORT_SIZE] = placement->source_width << 16 | HEIGHT;
	values[VIEWPORT_WIDTH_QUARTER] = placement->source_width / 4 - 1;
	values[HORIZONTAL_FILTER] = probe_gpu_registers[UNKNOWN_196C] | BUILT_IN_FILTER;
	values[HORIZONTAL_SCALE] = horizontal;
	values[HORIZONTAL_PHASE] = phase(horizontal);
	values[VERTICAL_FILTER] = probe_gpu_registers[UNKNOWN_1970] | BUILT_IN_FILTER;
	values[VERTICAL_SCALE] = vertical;
	values[VERTICAL_PHASE] = phase(vertical);
	values[OVERSCAN_LEFT_RIGHT] = placement->left << 16 | placement->right;
	values[OVERSCAN_TOP_BOTTOM] = placement->top << 16 | placement->bottom;
	values[CENTER] = 1;
}

// Under the scaler's lock, as src/display.c writes its set.
static void write_registers(const uint32_t *indices, const uint32_t *values, uint32_t count)
{
	probe_gpu_registers[AVIVO_D1SCL_UPDATE] = SCALER_UPDATE_LOCK;
	for (uint32_t i = 0; i < count; i++)
		probe_gpu_registers[indices[i]] = values[i];
	probe_gpu_registers[AVIVO_D1SCL_UPDATE] = 0;
}

static void place(const VIDEO_MODE *mode, Shape shape)
{
	const Placement placement = plan(mode, shape);
	xsdk_log_write(
		"letterbox: %u wide of the front buffer to %ux%u of the %ux%u output; bars left %u, right %u, top %u, "
		"bottom %u",
		(unsigned)placement.source_width, (unsigned)placement.width, (unsigned)placement.height,
		(unsigned)mode->display_width, (unsigned)mode->display_height, (unsigned)placement.left,
		(unsigned)placement.right, (unsigned)placement.top, (unsigned)placement.bottom);
	uint32_t values[WRITTEN];
	plan_values(&placement, values);
	write_registers(written, values, WRITTEN);
	(void)xsdk_thread_sleep(SETTLE_MS);
	for (uint32_t i = 0; i < WRITTEN; i++)
		xsdk_log_write("letterbox: wrote %04X (byte %04X) %08X, reads back %08X", (unsigned)written[i],
			(unsigned)(written[i] * 4), (unsigned)values[i], (unsigned)probe_gpu_registers[written[i]]);
}

static void wait_for_back(void)
{
	for (;;) {
		INPUT_STATE input;
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			return;
		(void)xsdk_thread_sleep(POLL_MS);
	}
}

static int show(Shape shape)
{
	log_modes();
	log_registers("at start");
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("letterbox: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_display_open(XSDK_DISPLAY_720P, &width, &height);
	if (!NT_SUCCESS(status) || width != WIDTH || height != HEIGHT) {
		xsdk_log_write("letterbox: FAILED, xsdk_display_open returned 0x%08X, %ux%u, where this needs xsdk's 1280x720",
			(unsigned)status, (unsigned)width, (unsigned)height);
		return 1;
	}
	volatile uint32_t *const pixels = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!pixels) {
		xsdk_log_write("letterbox: FAILED, no memory for the picture");
		return 1;
	}
	draw_picture(pixels, shape == PILLAR ? PILLAR_WIDTH : WIDTH);
	status = xsdk_display_show(xsdk_gpu_address(pixels), WIDTH);
	(void)xsdk_thread_sleep(SETTLE_MS);
	xsdk_log_write("letterbox: xsdk_display_show returned 0x%08X", (unsigned)status);
	log_registers("as xsdk shows it");
	uint32_t before[RESTORED];
	for (uint32_t i = 0; i < RESTORED; i++)
		before[i] = probe_gpu_registers[restored[i]];
	if (shape != FILL) {
		place(&mode, shape);
		log_registers("placed");
	}
	xsdk_log_write("letterbox: showing; Back on controller 1 ends it");
	wait_for_back();
	write_registers(restored, before, RESTORED);
	xsdk_display_close();
	xsdk_gpu_free(pixels);
	xsdk_log_write("letterbox: Back pressed; 1962, 1963 and 1967 put back to %08X %08X %08X, the display given back",
		(unsigned)before[0], (unsigned)before[1], (unsigned)before[2]);
	return 0;
}

static int read_case(void)
{
	log_modes();
	log_registers("read");
	return 0;
}

static int fill_case(void)
{
	return show(FILL);
}

static int letterbox_case(void)
{
	return show(LETTERBOX);
}

static int pillar_case(void)
{
	return show(PILLAR);
}

typedef struct {
	const char *name;
	const char *what;
	int (*run)(void);
} ProbeCase;

static const ProbeCase cases[] = {
	{ "read", "logs the display registers and modes, writing nothing", read_case },
	{ "fill", "shows 1280x720 as xsdk does, filling the output", fill_case },
	{ "letterbox", "shows 1280x720 across the output, bars above and below as on a 4:3 screen", letterbox_case },
	{ "pillar", "shows 960x720 at the output's height, bars left and right as on a 16:9 screen", pillar_case },
};

enum { CASES = sizeof cases / sizeof cases[0] };

static const ProbeCase *start_case(void)
{
	FILE *const file = fopen(case_path, "r");
	char name[NAME_BYTES] = "";
	const bool read = file && fscanf(file, "%15s", name) == 1;
	if (file)
		(void)fclose(file);
	for (uint32_t i = 0; read && i < CASES; i++) {
		if (!strcmp(name, cases[i].name))
			return &cases[i];
	}
	xsdk_log_write("letterbox: %s names no case, so read; the cases:", case_path);
	for (uint32_t i = 0; i < CASES; i++)
		xsdk_log_write("letterbox:   %s: %s", cases[i].name, cases[i].what);
	return &cases[0];
}

int main(void)
{
	const ProbeCase *const probe_case = start_case();
	xsdk_log_write("letterbox: case %s: %s", probe_case->name, probe_case->what);
	return probe_case->run();
}
