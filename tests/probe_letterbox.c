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
#include "xsdk/present.h"
#include "xsdk/thread.h"

// Milestone 54 step 5's probe (docs/scope/54-porters-files-and-system.md): letterboxing as xsdk does it, which places
// the scaled picture with radeonhd's overscan (src/display.c). game:\letterbox_case.txt names the case; without it,
// "read" runs. "read" logs the display controller's registers, VdQueryVideoMode and VdGetCurrentDisplayInformation,
// writing nothing, and ends. "fill" and "letterbox" present a test picture through xsdk_present every frame, as a title
// does, so XAM's VdSwap runs: a white border, 160-pixel squares from the centre, a ring of radius 300 there, and red,
// green, blue and yellow corners at the top left, top right, bottom left and bottom right. Square squares and a round
// ring mean unstretched; the border and four corners in view mean nothing is cut off. "fill" opens the display as a
// title does, filling a widescreen output; "letterbox" forces letterboxing, as on a 4:3 screen, with bars above and
// below. Each logs the registers before the display opens, once its frames are on screen, and after it closes. They
// run until Back on controller 1, or until the title is ended from outside. "kernel" writes nothing to the display: as
// "read", then it asks the kernel's own scaler builder, VdInitializeScalerCommandBuffer, for 1280x720 filling the
// output, letterboxed at 3/4 of its height and pillarboxed at 3/4 of its width, with the filters the kernel's own
// display information names. It logs each answer's count and every word, decoded as packets with the registers named,
// and what it wrote past the count or into the unknown argument's buffer, so the three answers show which registers
// place the picture. The packets aren't run.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	SQUARE = 160,
	// Keeps the squares' count from the centre positive.
	SQUARE_BIAS = 10 * SQUARE,
	RADIUS = 300,
	RING_HALF_WIDTH = 2,
	CORNER = 64,
	WHITE = 0xFFFFFFFF,
	DARK = 0xFF303030,
	LIGHT = 0xFF606060,
	RED = 0xFFFF0000,
	GREEN = 0xFF00FF00,
	BLUE = 0xFF0000FF,
	YELLOW = 0xFFFFFF00,
	CYAN = 0xFF00FFFF,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	BEGIN_TIMEOUT_MS = 1000,
	// Half a second at 60 Hz, then the registers are logged.
	LOG_AT_FRAME = 30,
	// Long enough for the display to take new values at a vblank.
	SETTLE_MS = 100,
	NAME_BYTES = 16,
	// Room for the scaler builder's packets, whose answer was 43 words (docs/HARDWARE.md), and for its unknown
	// argument.
	SCALER_WORDS = 1024,
	// What both buffers hold before each call, so the words the kernel wrote show.
	MARKER = 0xFEEDF00D,
	// In VdGetCurrentDisplayInformation's scaler parameters, each filter's type, its settings after it
	// (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:77-86).
	VERTICAL_FILTER_TYPE = 6,
	HORIZONTAL_FILTER_TYPE = 10,
	CHANGED_LOGGED = 64,
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

static const char *const case_path = "game:\\letterbox_case.txt";

static uint32_t scaler_words[SCALER_WORDS];
static uint32_t unknown_words[SCALER_WORDS];

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

static uint32_t picture_argb(uint32_t x, uint32_t y)
{
	const uint32_t right = WIDTH - 1;
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
	const int32_t dx = (int32_t)x - WIDTH / 2;
	const int32_t dy = (int32_t)y - HEIGHT / 2;
	const int32_t distance_squared = dx * dx + dy * dy;
	const int32_t inner = RADIUS - RING_HALF_WIDTH;
	const int32_t outer = RADIUS + RING_HALF_WIDTH;
	if (distance_squared >= inner * inner && distance_squared <= outer * outer)
		return CYAN;
	const uint32_t squares = (x + SQUARE_BIAS - WIDTH / 2) / SQUARE + (y + SQUARE_BIAS - HEIGHT / 2) / SQUARE;
	return squares & 1 ? LIGHT : DARK;
}

// The display reads a word's bytes as B, G, R, A (docs/HARDWARE.md, "A title's buffer on screen").
static void draw_picture(volatile uint32_t *pixels)
{
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++)
			pixels[tiled_index(x, y, WIDTH)] = __builtin_bswap32(picture_argb(x, y));
	}
	__asm__ volatile("sync" ::: "memory");
}

static bool back_pressed(void)
{
	INPUT_STATE input;
	return XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK;
}

// Each buffer gets the picture the first time it comes round, then keeps it; every frame goes through VdSwap.
static void present_until_back(void)
{
	for (uint32_t frame = 0; !back_pressed(); frame++) {
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write(
				"letterbox: FAILED, xsdk_present_begin returned 0x%08X at frame %u", (unsigned)status, (unsigned)frame);
			return;
		}
		if (frame < BUFFERS)
			draw_picture(xsdk_present_pixels(address));
		xsdk_present_end();
		if (frame == LOG_AT_FRAME) {
			XsdkPresentStats stats;
			xsdk_present_stats(&stats);
			xsdk_log_write("letterbox: %u flips over %u vblanks; Back on controller 1 ends it", (unsigned)stats.flips,
				(unsigned)stats.vblanks);
			log_registers("shown");
		}
	}
	xsdk_log_write("letterbox: Back pressed");
}

static int show(bool letterbox)
{
	log_modes();
	log_registers("at start");
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("letterbox: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	xsdk_display_force_letterbox(letterbox);
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	xsdk_display_force_letterbox(false);
	if (!NT_SUCCESS(status) || width != WIDTH || height != HEIGHT) {
		xsdk_log_write("letterbox: FAILED, xsdk_present_open returned 0x%08X, %ux%u, where this needs 1280x720",
			(unsigned)status, (unsigned)width, (unsigned)height);
		return 1;
	}
	present_until_back();
	xsdk_present_close();
	(void)xsdk_thread_sleep(SETTLE_MS);
	log_registers("closed");
	status = xsdk_gpu_close();
	xsdk_log_write("letterbox: done; xsdk_gpu_close returned 0x%08X", (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}

static const char *register_name(uint32_t index)
{
	for (uint32_t i = 0; i < LOGGED; i++) {
		if (logged[i].index == index)
			return logged[i].name;
	}
	return "";
}

static void log_packet_header(const char *label, uint32_t at, uint32_t header, uint32_t body)
{
	const uint32_t type = header >> 30;
	if (type == 0)
		xsdk_log_write("kernel: %s: %u: %08X, type 0, %u words from %04X%s", label, (unsigned)at, (unsigned)header,
			(unsigned)body, (unsigned)(header & 0x7FFF), header & 0x8000 ? ", all to it" : "");
	else if (type == 1)
		xsdk_log_write("kernel: %s: %u: %08X, type 1, to %04X and %04X", label, (unsigned)at, (unsigned)header,
			(unsigned)(header & 0x7FF), (unsigned)((header >> 11) & 0x7FF));
	else
		xsdk_log_write("kernel: %s: %u: %08X, type %u, opcode %02X, %u words", label, (unsigned)at, (unsigned)header,
			(unsigned)type, (unsigned)((header >> 8) & 0x7F), (unsigned)body);
}

// The packet at words[at], as Xenia reads PM4 (xenia:src/xenia/gpu/command_processor.cc:610-692); returns its words.
static uint32_t log_packet(const char *label, const uint32_t *words, uint32_t at, uint32_t count)
{
	const uint32_t header = words[at];
	const uint32_t type = header >> 30;
	const uint32_t body = type == 2 ? 0 : type == 1 ? 2 : ((header >> 16) & 0x3FFF) + 1;
	if (at + 1 + body > count) {
		xsdk_log_write("kernel: %s: %u: %08X, type %u of %u words, runs past the %u words returned", label,
			(unsigned)at, (unsigned)header, (unsigned)type, (unsigned)body, (unsigned)count);
		for (uint32_t i = at + 1; i < count; i++)
			xsdk_log_write("kernel: %s: %u:   %08X", label, (unsigned)i, (unsigned)words[i]);
		return count - at;
	}
	log_packet_header(label, at, header, body);
	for (uint32_t i = 0; i < body; i++) {
		const uint32_t value = words[at + 1 + i];
		if (type > 1) {
			xsdk_log_write("kernel: %s: %u:   %08X", label, (unsigned)(at + 1 + i), (unsigned)value);
			continue;
		}
		const uint32_t index = type == 1 ? (header >> (11 * i)) & 0x7FF : (header & 0x7FFF) + (header & 0x8000 ? 0 : i);
		xsdk_log_write("kernel: %s: %u:   %08X to %04X (byte %04X) %s", label, (unsigned)(at + 1 + i), (unsigned)value,
			(unsigned)index, (unsigned)(index * 4), register_name(index));
	}
	return 1 + body;
}

// The words of a buffer, filled with MARKER before the call, that the kernel changed from first on.
static void log_changed(const char *label, const char *what, const uint32_t *words, uint32_t first)
{
	uint32_t changed = 0;
	for (uint32_t i = first; i < SCALER_WORDS; i++) {
		if (words[i] == MARKER)
			continue;
		if (changed < CHANGED_LOGGED)
			xsdk_log_write("kernel: %s: %s word %u: %08X", label, what, (unsigned)i, (unsigned)words[i]);
		changed++;
	}
	xsdk_log_write("kernel: %s: %u %s words changed", label, (unsigned)changed, what);
}

// Asks the kernel for the packets that show 1280x720 at an output rectangle, and logs them; nothing runs them.
static void ask_kernel(
	const char *label, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const DISPLAY_INFO *info)
{
	for (uint32_t i = 0; i < SCALER_WORDS; i++) {
		scaler_words[i] = MARKER;
		unknown_words[i] = MARKER;
	}
	const uint32_t *const parameters = info->scaler_parameters;
	D3DFILTER_PARAMETERS vertical;
	D3DFILTER_PARAMETERS horizontal;
	memcpy(&vertical, &parameters[VERTICAL_FILTER_TYPE + 1], sizeof vertical);
	memcpy(&horizontal, &parameters[HORIZONTAL_FILTER_TYPE + 1], sizeof horizontal);
	const uint32_t returned = VdInitializeScalerCommandBuffer(0, HEIGHT << 16 | WIDTH, y << 16 | x,
		height << 16 | width, HEIGHT << 16 | WIDTH, parameters[VERTICAL_FILTER_TYPE], &vertical,
		parameters[HORIZONTAL_FILTER_TYPE], &horizontal, unknown_words, scaler_words, SCALER_WORDS);
	xsdk_log_write("kernel: %s: 1280x720 to %ux%u at %u,%u, filter types %u and %u; returned %u", label,
		(unsigned)width, (unsigned)height, (unsigned)x, (unsigned)y, (unsigned)parameters[VERTICAL_FILTER_TYPE],
		(unsigned)parameters[HORIZONTAL_FILTER_TYPE], (unsigned)returned);
	const uint32_t count = returned < SCALER_WORDS ? returned : SCALER_WORDS;
	for (uint32_t at = 0; at < count;)
		at += log_packet(label, scaler_words, at, count);
	log_changed(label, "packet buffer's, past the count,", scaler_words, count);
	log_changed(label, "unknown argument's", unknown_words, 0);
}

static int kernel_case(void)
{
	log_modes();
	log_registers("read");
	VIDEO_MODE mode;
	VdQueryVideoMode(&mode);
	DISPLAY_INFO info;
	VdGetCurrentDisplayInformation(&info);
	const uint32_t output_width = mode.display_width;
	const uint32_t output_height = mode.display_height;
	// As src/display.c plans them: 3/4 of the output, a multiple of 4, centred.
	const uint32_t letterbox_height = (output_height * 3 / 4) & ~3u;
	const uint32_t pillarbox_width = (output_width * 3 / 4) & ~3u;
	ask_kernel("fill", 0, 0, output_width, output_height, &info);
	ask_kernel("letterbox", 0, (output_height - letterbox_height) / 2, output_width, letterbox_height, &info);
	ask_kernel("pillarbox", (output_width - pillarbox_width) / 2, 0, pillarbox_width, output_height, &info);
	log_registers("after");
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
	return show(false);
}

static int letterbox_case(void)
{
	return show(true);
}

typedef struct {
	const char *name;
	const char *what;
	int (*run)(void);
} ProbeCase;

static const ProbeCase cases[] = {
	{ "read", "logs the display registers and modes, writing nothing", read_case },
	{ "fill", "presents 1280x720 as a title does, filling a widescreen output", fill_case },
	{ "letterbox", "presents 1280x720 letterboxed, forced, as on a 4:3 screen", letterbox_case },
	{ "kernel", "asks the kernel's scaler builder to fill, letterbox and pillarbox, writing nothing", kernel_case },
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
