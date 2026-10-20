#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constant_shaders.h"
#include "hiz_shaders.h"
#include "probe_gpu.h"
#include "tile_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Milestone 44 step 1: what tiling needs of the GPU that xsdk doesn't send yet (docs/scope/44-tiling-and-msaa.md).
// game:\tiles_case.txt holds a case's number or name: 1 offset, 2 position, 3 resolve and 4 depth run from the one
// named to 4, then 11, all five without the file; 5 bins_high_first, 6 bins_low_first, 7 bins_one_word,
// 8 bins_recorded, 9 bins_unset and 10 bins_exit send packets xsdk never has, so each runs alone, only when named, from
// a fresh boot. Step 2's 11 cost times xsdk's tiled passes, which send only what cases 1 to 5 proved; named, it runs
// alone.
#ifndef TILES_BREAK
#define TILES_BREAK ""
#endif

enum {
	WIDTH = 256,
	HEIGHT = 128,
	// The window offset's band: target rows 64 to 127, which the offset moves up to eDRAM rows 0 to 63.
	BAND_TOP = 64,
	BAND_ROWS = 64,
	BAND_PIXELS = WIDTH * BAND_ROWS,
	// The resolve's and depth's bands, as a tiled texture's base moves by 32 rows
	// (xenia:src/xenia/gpu/xenos.h:1160-1173).
	STEP_ROWS = 32,
	STEPS = HEIGHT / STEP_ROWS,
	STEP_PIXELS = WIDTH * STEP_ROWS,
	// Case 3's texture: a step of sentinels past the last band.
	TEXTURE_ROWS = HEIGHT + STEP_ROWS,
	FENCE_TIMEOUT_MS = 1000,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	SQUARE_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	CLOCK_BYTES = XSDK_GPU_CLOCK_WORDS * 4,
	// A row's colour: red the row, green and blue marking it as one; the position shader's and the cells' blue.
	ROW_GREEN = 0xA0,
	ROW_BLUE = 0x30,
	POSITION_BLUE = 0x50,
	CELL_BLUE = 0xC0,
	CHANNEL_MASK = 0xFF,
	// What the CPU fills an image with before a resolve; a resolved colour's alpha is never 0.
	MARKER = 0,
	// Case 4: depth whole from tile 2036, so band 1's 8 tiles run past tile 2048 and wrap to 0
	// (xenia:src/xenia/gpu/xenos.h:271-276); the band's colour apart from it; and where the hierarchical depth's runs
	// keep depth, clear of the wrap, as xsdk's resolve clears only a target that fits.
	WRAP_DEPTH_TILE = 2036,
	BAND_COLOR_TILE = 512,
	SPARE_DEPTH_TILE = 1536,
	HIZ_DEPTH_TILE = 1024,
	HIDDEN_QUADS = 8,
	// eDRAM's tiles of 80x16 samples (xenia:src/xenia/gpu/xenos.h:414-415).
	TILE_COLUMNS = 80,
	TILE_ROWS = 16,
	// Bit 18 hid 512 pixels at the end of a 1280x720 target (docs/HARDWARE.md, "Hierarchical depth, milestone 30").
	HIZ_PIXELS_A_STEP = 512,
	// The GPU's clock counts 1995 every 4 us (docs/HARDWARE.md, "Queries and events").
	CLOCKS_IN_4_US = 1995,
	// Case 5's cells: a row for each select, a column for each mask.
	SELECTS = 3,
	MASKS = 4,
	CELL_WIDTH = WIDTH / MASKS,
	CELL_HEIGHT = 32,
	CELL_PIXELS = CELL_WIDTH * CELL_HEIGHT,
	RECORD_DWORDS = 4096,
	SAFE_CASES = 4,
	COST_CASE = 10,
	CASE_NAME_BYTES = 32,
	TEXT_BYTES = 160,
	LINE_BYTES = 512,
};

// Colours as ARGB; a resolve stores them byte-swapped (stored_of).
enum {
	SENTINEL_ARGB = 0xFF202020,
	FILL_ARGB = 0xFF406080,
	NEAR_ARGB = 0xFF40E040,
	FAR_ARGB = 0xFFE04040,
	OPAQUE = 0xFF000000,
};

// Registers by index (xenia:src/xenia/gpu/register_table.inc:482-497, 539).
enum {
	RB_DEPTH_INFO = 0x2002,
	PA_SC_SCREEN_SCISSOR_TL = 0x200E,
	PA_SC_SCREEN_SCISSOR_BR = 0x200F,
	PA_SC_WINDOW_OFFSET = 0x2080,
	PA_SC_WINDOW_SCISSOR_TL = 0x2081,
	PA_SC_WINDOW_SCISSOR_BR = 0x2082,
	RB_HIZCONTROL = 0x2203,
};

// Case 11's frames: 1280x720, one 8_8_8_8 target and depth, untiled, then in 1, 2 and 3 bands.
enum {
	COST_WIDTH = 1280,
	COST_HEIGHT = 720,
	COST_TILINGS = 4,
	FILL_QUADS = 4,
	VERTEX_DRAWS = 16,
	// Whole triangles, under the draw's 16-bit count (xenia:src/xenia/gpu/registers.h:309-327).
	VERTEX_COUNT = 65532,
	VERTEX_BYTES = VERTEX_COUNT * STRIDE_BYTES,
	// The second run of each frame is timed, as its pass finds band 0 cleared by the first.
	COST_RUNS = 2,
	// A band's fixed cost should stay under this, past the resolve's copying (docs/scope/44-tiling-and-msaa.md).
	FIXED_COST_BAR_US = 50,
	// A predicated draw outside its band should cost under a fifth of the same draw drawn.
	SKIPPED_SHARE = 5,
};

typedef enum { SCENE_FILL, SCENE_VERTEX, SCENE_EMPTY } CostScene;

static const char *const cost_tiling_names[COST_TILINGS] = { "untiled", "1 band", "2 bands", "3 bands" };
// 0 untiled, else a band's rows.
static const uint32_t cost_band_rows[COST_TILINGS] = { 0, 736, 384, 256 };
// The multisampled frames the scope times (docs/scope/44-tiling-and-msaa.md, "Done when"): 2 samples in 2 and 3 bands,
// 4 in 3; 1280x720 doesn't fit eDRAM untiled at either.
static const struct {
	uint32_t samples;
	uint32_t band_rows;
} cost_multisampled[] = { { 2, 384 }, { 2, 256 }, { 4, 256 } };

// Type-3 opcodes (xenia:src/xenia/gpu/xenos.h:1601, 1618-1619, 1630-1633).
enum {
	PM4_DRAW_INDX_2 = 0x36,
	PM4_SET_BIN_MASK = 0x50,
	PM4_SET_BIN_SELECT = 0x51,
	PM4_SET_BIN_MASK_LO = 0x60,
	PM4_SET_BIN_SELECT_LO = 0x62,
};

enum {
	// The window offset's and the scissors' y from bit 16, the offset 15 bits signed each way
	// (xenia:src/xenia/gpu/registers.h:586-633).
	Y_SHIFT = 16,
	OFFSET_MASK = 0x7FFF,
	// D3D's screen scissor, which xsdk's set-up leaves (src/gpu_setup.c; xenia:src/xenia/gpu/draw_util.cc:574-575).
	SCREEN_WHOLE = 8192,
	// RB_HIZCONTROL's write and test, and the bits that seemed to move the buffer (docs/HARDWARE.md, "Hierarchical
	// depth, milestone 30").
	HIZ_WRITE_ENABLE = 1 << 0,
	HIZ_ENABLE = 1 << 1,
	HIZ_MOVE_SHIFT = 18,
	// A type-3 header's bit 0 runs the packet only if BIN_SELECT & BIN_MASK isn't 0
	// (xenia:src/xenia/gpu/command_processor.cc:704-713).
	PREDICATE = 1,
	// DRAW_INDX_2's initiator, auto-indexed, as xsdk_gpu_draw sends it (xenia:src/xenia/gpu/registers.h:309-327).
	AUTO_INDEX = 2 << 6,
	VERTEX_COUNT_SHIFT = 16,
};

// The window scissor's window_offset_disable, which keeps it where it is (xenia:src/xenia/gpu/registers.h:622-633).
enum { WINDOW_OFFSET_DISABLE = 0x80000000 };

// How a case sends the bin registers.
enum {
	BINS_HIGH_FIRST,
	BINS_LOW_FIRST,
	BINS_ONE_WORD,
	BINS_RECORDED,
	BINS_NONE,
};

static const char *const bin_form_names[] = {
	"PM4_SET_BIN_MASK and PM4_SET_BIN_SELECT (0x50, 0x51), high word first",
	"PM4_SET_BIN_MASK and PM4_SET_BIN_SELECT (0x50, 0x51), low word first",
	"PM4_SET_BIN_MASK_LO to PM4_SET_BIN_SELECT_HI (0x60 to 0x63), a word each",
	"PM4_SET_BIN_MASK and PM4_SET_BIN_SELECT (0x50, 0x51), high word first, in a recording run with "
	"PM4_INDIRECT_BUFFER",
	"no bin packet",
};

typedef enum { SKIPPED, LANDED, PARTLY, OUTCOMES } Outcome;

static const char *const outcome_names[OUTCOMES] = { "skipped", "landed", "partly drawn" };

static const float NEAR_DEPTH = 0.25f;
static const float FAR_DEPTH = 0.5f;
static const float FILL_DEPTH = 0.875f;

static const uint64_t ALL_BINS = UINT64_MAX;
static const uint64_t bin_selects[SELECTS] = { 1, 2, 1ull << 32 };
static const uint64_t bin_masks[MASKS] = { 1, 2, 1ull << 32, UINT64_MAX };

static const char *const start_path = "game:\\tiles_case.txt";

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
} Rect;

typedef struct ProbeCase ProbeCase;

struct ProbeCase {
	const char *name;
	const char *what;
	bool (*run)(const ProbeCase *c);
	uint32_t form;
	const uint64_t *selects;
	uint32_t select_count;
};

// Case 1's window scissors: rows in the target, or in eDRAM if fixed, and the eDRAM rows the draw belongs in.
typedef struct {
	const char *name;
	uint32_t top;
	uint32_t bottom;
	bool fixed;
	uint32_t first;
	uint32_t end;
} ScissorCase;

static const ScissorCase scissor_cases[] = {
	{ "the window scissor over the whole target", 0, HEIGHT, false, 0, BAND_ROWS },
	{ "the window scissor at target rows 32-95", 32, 96, false, 0, 32 },
	{ "the window scissor at rows 32-95 with window_offset_disable", 32, 96, true, 32, BAND_ROWS },
};

// Rows first to end - 1 of an image hold row y + shift's colour, the others the sentinel.
typedef struct {
	uint32_t rows;
	uint32_t first;
	uint32_t end;
	uint32_t shift;
} RowLayout;

typedef struct {
	uint32_t right;
	uint32_t missing;
	uint32_t stray;
	uint32_t misplaced;
	uint32_t other;
	// The rows found, and the image rows they were found at.
	uint32_t found_low;
	uint32_t found_high;
	uint32_t at_low;
	uint32_t at_high;
} RowCount;

typedef uint32_t (*ExpectedArgb)(uint32_t x, uint32_t y);

// Case 4's quads in the whole target: the near one over bands 0 to 2, the far one over all four.
static const Rect near_rect = { 32, 16, 128, 64 };
static const Rect far_rect = { 96, 8, 128, 112 };

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

static XsdkShader vertex_shader;
static XsdkShader flat_shader;
static XsdkShader position_shader;
static XsdkShader long_shader;
static uint32_t square_address;
static volatile uint32_t *image;
static volatile uint32_t *band_images[STEPS];
static volatile uint32_t *clock_words;
static XsdkCommandBuffer commands;
static XsdkTexture cost_frame;
static uint32_t cost_vertices;
// The target draw_quad places quads in, and the form put_bins sends.
static uint32_t place_width;
static uint32_t place_height;
static uint32_t bin_form;
static uint32_t passed;
static uint32_t differed;

// Whether this is the break build named: nopred, flip or nomove, each failing the check it aims at.
static bool broken(const char *name)
{
	return strcmp(TILES_BREAK, name) == 0;
}

static const char *verdict(bool pass)
{
	passed += pass;
	differed += !pass;
	return pass ? "PASS" : "DIFFERS";
}

static uint32_t stored_of(uint32_t argb)
{
	return __builtin_bswap32(argb);
}

static uint32_t row_argb(uint32_t row)
{
	return OPAQUE | row << 16 | ROW_GREEN << 8 | ROW_BLUE;
}

static uint32_t cell_argb(uint32_t select, uint32_t mask)
{
	return OPAQUE | (0x30 + 0x40 * mask) << 16 | (0x30 + 0x40 * select) << 8 | CELL_BLUE;
}

static Rect cell_rect(uint32_t select, uint32_t mask)
{
	return (Rect){ mask * CELL_WIDTH, select * CELL_HEIGHT, CELL_WIDTH, CELL_HEIGHT };
}

static Rect band_rect(uint32_t band)
{
	return (Rect){ 0, band * STEP_ROWS, WIDTH, STEP_ROWS };
}

// A quarter into the channel's 8-bit step, so rounding and truncating both give the byte back.
static float channel(uint32_t value)
{
	return ((value & CHANNEL_MASK) + 0.25f) / 255.0f;
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

// The constant vertex shader places the unit square over the rectangle at the depth, coloured (tests/probe_hiz.c).
static void place_quad(const Rect *r, float depth, uint32_t argb)
{
	const float w = (float)place_width;
	const float h = (float)place_height;
	const float constants[3][4] = {
		{ 2.0f * r->width / w, -2.0f * r->height / h, 1.0f, 1.0f },
		{ -1.0f - 1.0f / w + 2.0f * r->x / w, 1.0f + 1.0f / h - 2.0f * r->y / h, depth, 0.0f },
		{ channel(argb >> 16), channel(argb >> 8), channel(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &constants[0][0], 3);
}

static void draw_quad(const Rect *r, float depth, uint32_t argb)
{
	place_quad(r, depth, argb);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// Rows 0 to count - 1 of the target, a quad each, row r in row first + r's colour.
static void draw_rows(uint32_t first, uint32_t count)
{
	for (uint32_t r = 0; r < count; r++)
		draw_quad(&(Rect){ 0, r, WIDTH, 1 }, 0.0f, row_argb(first + r));
}

static void set_shaders(const XsdkShader *pixel)
{
	xsdk_gpu_set_vertices(0, square_address, SQUARE_BYTES);
	xsdk_gpu_set_shaders(&vertex_shader, pixel);
}

// The target and what every draw takes, as a resolve leaves them to be set again.
static void set_state(uint32_t width, uint32_t height, const XsdkShader *pixel)
{
	xsdk_gpu_set_target(width, height);
	place_width = width;
	place_height = height;
	set_shaders(pixel);
}

static int32_t band_offset(void)
{
	return broken("flip") ? BAND_TOP : -BAND_TOP;
}

// The band's rows moved up to eDRAM's top and the screen scissor at its size, as a tile is drawn
// (docs/scope/44-tiling-and-msaa.md, "A tile's offset"; xenia:src/xenia/gpu/draw_util.cc:339-342, 559-581).
static void put_band_window(void)
{
	put_register(PA_SC_WINDOW_OFFSET, ((uint32_t)band_offset() & OFFSET_MASK) << Y_SHIFT);
	put_register(PA_SC_SCREEN_SCISSOR_TL, 0);
	put_register(PA_SC_SCREEN_SCISSOR_BR, WIDTH | BAND_ROWS << Y_SHIFT);
}

// The screen scissor whole again, as xsdk leaves it: Xenia clips resolves to it too (draw_util.cc:576-581, 883).
static void put_screen_whole(void)
{
	put_register(PA_SC_SCREEN_SCISSOR_TL, 0);
	put_register(PA_SC_SCREEN_SCISSOR_BR, SCREEN_WHOLE | SCREEN_WHOLE << Y_SHIFT);
}

// Submits what's queued and waits for it; false, logged, if the GPU didn't finish.
static bool finish(const char *what)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("tiles probe: %s: the fence didn't land: 0x%08X", what, (unsigned)status);
	probe_report_cp("tiles probe", what);
	return false;
}

// Marks the memory, so a pixel the resolve misses shows, then resolves the target's top rows into it and waits.
static bool resolve_into(volatile uint32_t *memory, uint32_t rows, const char *what)
{
	const uint32_t words = xsdk_gpu_resolve_bytes(WIDTH, rows) / 4;
	for (uint32_t i = 0; i < words; i++)
		memory[i] = MARKER;
	__asm__ volatile("sync" ::: "memory");
	put_screen_whole();
	xsdk_gpu_resolve(xsdk_gpu_address(memory), WIDTH, rows, SENTINEL_ARGB);
	return finish(what);
}

// A pixel's row, or -1 if it holds no row's colour; a resolve stores blue, green, red, alpha from the top byte.
static int32_t row_of(uint32_t stored)
{
	const bool row = (stored & CHANNEL_MASK) == CHANNEL_MASK && (stored >> 16 & CHANNEL_MASK) == ROW_GREEN &&
		stored >> 24 == ROW_BLUE;
	return row ? (int32_t)(stored >> 8 & CHANNEL_MASK) : -1;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

static void count_pixel(RowCount *n, const RowLayout *layout, uint32_t y, uint32_t stored)
{
	const int32_t row = row_of(stored);
	const bool belongs = y >= layout->first && y < layout->end;
	if (row >= 0) {
		n->found_low = min_u32(n->found_low, (uint32_t)row);
		n->found_high = max_u32(n->found_high, (uint32_t)row);
		n->at_low = min_u32(n->at_low, y);
		n->at_high = max_u32(n->at_high, y);
	}
	if (belongs ? row == (int32_t)(y + layout->shift) : stored == stored_of(SENTINEL_ARGB))
		n->right++;
	else if (belongs && stored == stored_of(SENTINEL_ARGB))
		n->missing++;
	else if (row >= 0 && belongs)
		n->misplaced++;
	else if (row >= 0)
		n->stray++;
	else
		n->other++;
}

static void count_rows(volatile const uint32_t *memory, const RowLayout *layout, RowCount *n)
{
	*n = (RowCount){ .found_low = UINT32_MAX, .at_low = UINT32_MAX };
	for (uint32_t y = 0; y < layout->rows; y++) {
		for (uint32_t x = 0; x < WIDTH; x++)
			count_pixel(n, layout, y, memory[tiled_index(x, y, WIDTH)]);
	}
}

static void log_rows(const char *what, const char *place, const RowLayout *layout, const RowCount *n)
{
	char found[TEXT_BYTES] = "no row's colour found";
	if (n->found_low <= n->found_high)
		snprintf(found, sizeof found, "found rows %u-%u at %s rows %u-%u", (unsigned)n->found_low,
			(unsigned)n->found_high, place, (unsigned)n->at_low, (unsigned)n->at_high);
	xsdk_log_write(
		"tiles probe: %s: rows %u-%u belong at %s rows %u-%u, the sentinel elsewhere; %s; %u right, %u missing, "
		"%u stray, %u misplaced, %u other; %s",
		what, (unsigned)(layout->first + layout->shift), (unsigned)(layout->end - 1 + layout->shift), place,
		(unsigned)layout->first, (unsigned)(layout->end - 1), found, (unsigned)n->right, (unsigned)n->missing,
		(unsigned)n->stray, (unsigned)n->misplaced, (unsigned)n->other, verdict(n->right == layout->rows * WIDTH));
}

// xsdk's window scissor, or the same rows with window_offset_disable, which keeps it from moving with the offset.
static void put_window_scissor(const ScissorCase *s)
{
	if (!s->fixed) {
		xsdk_gpu_set_scissor(0, s->top, WIDTH, s->bottom - s->top);
		return;
	}
	put_register(PA_SC_WINDOW_SCISSOR_TL, s->top << Y_SHIFT | WINDOW_OFFSET_DISABLE);
	put_register(PA_SC_WINDOW_SCISSOR_BR, WIDTH | s->bottom << Y_SHIFT);
}

// Every row of the target in its own colour under the band's window, with each window scissor; each row must land
// BAND_TOP rows up, inside the scissors, and nowhere else.
static bool run_offset(const ProbeCase *c)
{
	(void)c;
	for (uint32_t i = 0; i < sizeof scissor_cases / sizeof scissor_cases[0]; i++) {
		const ScissorCase *const s = &scissor_cases[i];
		char what[TEXT_BYTES];
		snprintf(what, sizeof what, "case 1 offset, window offset (0,%d), screen scissor %ux%u, %s", (int)band_offset(),
			(unsigned)WIDTH, (unsigned)BAND_ROWS, s->name);
		set_state(WIDTH, HEIGHT, &flat_shader);
		put_band_window();
		put_window_scissor(s);
		draw_rows(0, HEIGHT);
		if (!resolve_into(image, HEIGHT, what))
			return false;
		const RowLayout layout = { HEIGHT, s->first, s->end, BAND_TOP };
		RowCount n;
		count_rows(image, &layout, &n);
		log_rows(what, "eDRAM", &layout, &n);
	}
	return true;
}

// The position shader's colour: |x| in red and |y| in green, blue a mark (tools/gen_tile_shaders.py).
static bool position_is(uint32_t stored, uint32_t x, uint32_t y)
{
	return (stored & CHANNEL_MASK) == CHANNEL_MASK && stored >> 24 == POSITION_BLUE &&
		(stored >> 8 & CHANNEL_MASK) == x && (stored >> 16 & CHANNEL_MASK) == y;
}

// Whether gl_FragCoord.y, the pixel parameters' |y| under xost's SC_OUTPUT_SCREEN_XY, reads the row in the band or in
// the target: Xenia gives the band's (xenia:src/xenia/gpu/dxbc_shader_translator.cc:705-725).
static bool run_position(const ProbeCase *c)
{
	(void)c;
	const float constants[2][4] = {
		{ 1.0f / 255.0f, 1.0f / 255.0f, 0.0f, 0.0f },
		{ channel(0), channel(0), channel(POSITION_BLUE), 1.0f },
	};
	set_state(WIDTH, HEIGHT, &position_shader);
	xsdk_gpu_set_pixel_constants(0, &constants[0][0], 2);
	put_band_window();
	draw_quad(&(Rect){ 0, 0, WIDTH, HEIGHT }, 0.0f, OPAQUE);
	if (!resolve_into(image, HEIGHT, "case 2 position"))
		return false;
	uint32_t band = 0;
	uint32_t target = 0;
	uint32_t neither = 0;
	uint32_t stray = 0;
	char first[TEXT_BYTES] = "";
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = image[tiled_index(x, y, WIDTH)];
			if (y >= BAND_ROWS) {
				stray += stored != stored_of(SENTINEL_ARGB);
			} else if (position_is(stored, x, y)) {
				band++;
			} else if (position_is(stored, x, y + BAND_TOP)) {
				target++;
			} else if (!neither++) {
				snprintf(first, sizeof first, ", the first at eDRAM (%u,%u) holding %08X", (unsigned)x, (unsigned)y,
					(unsigned)stored);
			}
		}
	}
	const char *const answer = band == BAND_PIXELS ? "gl_FragCoord.y reads the row in the band"
		: target == BAND_PIXELS                    ? "gl_FragCoord.y reads the row in the target"
												   : "gl_FragCoord.y reads neither throughout";
	xsdk_log_write(
		"tiles probe: case 2 position, window offset (0,%d): of %u pixels in eDRAM rows 0-%u, %u hold their row in "
		"the band (0-%u), %u their row in the target (%u-%u), %u neither%s; %u stray below them; %s; %s",
		(int)band_offset(), (unsigned)BAND_PIXELS, (unsigned)(BAND_ROWS - 1), (unsigned)band, (unsigned)(BAND_ROWS - 1),
		(unsigned)target, (unsigned)BAND_TOP, (unsigned)(HEIGHT - 1), (unsigned)neither, first, (unsigned)stray, answer,
		verdict(!stray && (band == BAND_PIXELS || target == BAND_PIXELS)));
	return true;
}

// Bands of 32 rows, each drawn at the target's top and resolved with RB_COPY_DEST_BASE moved down to its rows of a
// texture of sentinels; each band must land at its rows, the rows above and below left alone.
static bool run_resolve(const ProbeCase *c)
{
	(void)c;
	const uint32_t words = xsdk_gpu_resolve_bytes(WIDTH, TEXTURE_ROWS) / 4;
	for (uint32_t i = 0; i < words; i++)
		image[i] = stored_of(SENTINEL_ARGB);
	__asm__ volatile("sync" ::: "memory");
	for (uint32_t step = 1; step < STEPS; step++) {
		const uint32_t row = step * STEP_ROWS;
		// Row y, column 0 of a tiled 32-bit texture is y * pitch * 4 bytes in, for y a multiple of 32
		// (xenia:src/xenia/gpu/texture_address.h:190-207).
		const uint32_t moved = broken("nomove") ? 0 : row * WIDTH * 4;
		char what[TEXT_BYTES];
		snprintf(what, sizeof what,
			"case 3 resolve, a %ux%u band resolved with RB_COPY_DEST_BASE %u bytes into a %ux%u tiled texture",
			(unsigned)WIDTH, (unsigned)STEP_ROWS, (unsigned)moved, (unsigned)WIDTH, (unsigned)TEXTURE_ROWS);
		set_state(WIDTH, STEP_ROWS, &flat_shader);
		draw_rows(row, STEP_ROWS);
		xsdk_gpu_resolve(xsdk_gpu_address(image) + moved, WIDTH, STEP_ROWS, SENTINEL_ARGB);
		if (!finish(what))
			return false;
		const RowLayout layout = { TEXTURE_ROWS, STEP_ROWS, row + STEP_ROWS, 0 };
		RowCount n;
		count_rows(image, &layout, &n);
		log_rows(what, "texture", &layout, &n);
	}
	return true;
}

// A band of 32 rows with its depth at depth_tile, which may run past tile 2048, its rows placed in the whole target
// by the viewport.
static void set_band(uint32_t band, uint32_t color_tile, uint32_t depth_tile, const XsdkShader *pixel)
{
	// xsdk asserts a target's depth fits below tile 2048, so the band's own base goes in after.
	xsdk_gpu_set_target_at(WIDTH, STEP_ROWS, color_tile, SPARE_DEPTH_TILE);
	put_register(RB_DEPTH_INFO, depth_tile);
	xsdk_gpu_set_viewport(0.0f, -(float)(band * STEP_ROWS), WIDTH, HEIGHT, 0.0f, 1.0f);
	place_width = WIDTH;
	place_height = HEIGHT;
	set_shaders(pixel);
}

// xsdk's depth setting, then the probe's hierarchical depth control, as xsdk's follows only targets it cleared.
static void set_depth(uint32_t compare, bool write, uint32_t hiz_control)
{
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = write, .depth_compare = compare };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
	put_register(RB_HIZCONTROL, hiz_control);
}

static bool inside(const Rect *r, uint32_t x, uint32_t y)
{
	return x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
}

static uint32_t near_over_fill(uint32_t x, uint32_t y)
{
	return inside(&near_rect, x, y) ? NEAR_ARGB : FILL_ARGB;
}

static uint32_t far_behind_near(uint32_t x, uint32_t y)
{
	return inside(&far_rect, x, y) && !inside(&near_rect, x, y) ? FAR_ARGB : SENTINEL_ARGB;
}

static uint32_t far_past_band_0(uint32_t x, uint32_t y)
{
	(void)x;
	return y < STEP_ROWS ? SENTINEL_ARGB : FAR_ARGB;
}

// The band's pixels that aren't what's expected at their place in the whole target; the first goes into text.
static uint32_t count_wrong(uint32_t band, ExpectedArgb expected, char *text, size_t size)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < STEP_ROWS; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = band_images[band][tiled_index(x, y, WIDTH)];
			const uint32_t want = stored_of(expected(x, band * STEP_ROWS + y));
			if (stored == want)
				continue;
			if (!wrong)
				snprintf(text, size, ", the first at (%u,%u) holding %08X for %08X", (unsigned)x,
					(unsigned)(band * STEP_ROWS + y), (unsigned)stored, (unsigned)want);
			wrong++;
		}
	}
	return wrong;
}

static void describe_tiles(char *text, size_t size, uint32_t tile, uint32_t count)
{
	if (tile + count <= XSDK_EDRAM_TILES)
		snprintf(text, size, "tiles %u-%u", (unsigned)tile, (unsigned)(tile + count - 1));
	else
		snprintf(text, size, "tiles %u-%u and 0-%u", (unsigned)tile, (unsigned)(XSDK_EDRAM_TILES - 1),
			(unsigned)(tile + count - XSDK_EDRAM_TILES - 1));
}

// One pass over every band with depth whole: first a fill and the near quad, then the far quad, which must hide behind
// the near one's depth from the pass before, each band's depth left in eDRAM between them.
static bool depth_pass(uint32_t pass, uint32_t band_tiles)
{
	static const char *const pass_names[] = {
		"depth whole, pass 1: a fill at 0.875 written always, then the near quad at 0.25 less-than",
		"depth whole, pass 2: the far quad at 0.5 less-than, hidden where pass 1's near quad is",
	};
	uint32_t wrong_total = 0;
	for (uint32_t band = 0; band < STEPS; band++) {
		const uint32_t tile = (WRAP_DEPTH_TILE + band * band_tiles) % XSDK_EDRAM_TILES;
		char tiles[TEXT_BYTES];
		describe_tiles(tiles, sizeof tiles, tile, band_tiles);
		const Rect rows = band_rect(band);
		set_band(band, BAND_COLOR_TILE, tile, &flat_shader);
		if (pass == 0) {
			set_depth(XSDK_COMPARE_ALWAYS, true, 0);
			draw_quad(&rows, FILL_DEPTH, FILL_ARGB);
			set_depth(XSDK_COMPARE_LESS, true, 0);
			draw_quad(&near_rect, NEAR_DEPTH, NEAR_ARGB);
		} else {
			set_depth(XSDK_COMPARE_LESS, true, 0);
			draw_quad(&far_rect, FAR_DEPTH, FAR_ARGB);
		}
		if (!resolve_into(band_images[band], STEP_ROWS, pass_names[pass]))
			return false;
		char first[TEXT_BYTES] = "";
		const uint32_t wrong = count_wrong(band, pass ? far_behind_near : near_over_fill, first, sizeof first);
		wrong_total += wrong;
		xsdk_log_write(
			"tiles probe: case 4 %s: band %u, rows %u-%u, depth at RB_DEPTH_INFO %u, %s: %u of %u pixels "
			"wrong%s",
			pass_names[pass], (unsigned)band, (unsigned)(band * STEP_ROWS), (unsigned)((band + 1) * STEP_ROWS - 1),
			(unsigned)tile, tiles, (unsigned)wrong, (unsigned)STEP_PIXELS, first);
	}
	xsdk_log_write("tiles probe: case 4 %s: %u pixels wrong in all; %s", pass_names[pass], (unsigned)wrong_total,
		verdict(!wrong_total));
	return true;
}

static uint32_t clock_us(uint64_t clocks)
{
	return (uint32_t)(clocks * 4 / CLOCKS_IN_4_US);
}

// The whole target's depth cleared with the hierarchical buffer, as xsdk clears them; then band 0's depth near, written
// into the buffer, and in each band in turn hidden quads through the long shader, timed, with the buffer tested and
// RB_HIZCONTROL's bits 18-26 at the band's number times step. Band 0's quads are hidden and the others' drawn.
static bool hiz_run(const char *name, uint32_t step, uint32_t band_tiles)
{
	xsdk_gpu_set_resolve_clears(true, true);
	xsdk_gpu_set_target_at(WIDTH, HEIGHT, 0, HIZ_DEPTH_TILE);
	if (!resolve_into(image, HEIGHT, "case 4 depth, the whole target's clear"))
		return false;
	xsdk_gpu_set_resolve_clears(true, false);
	const Rect top = band_rect(0);
	set_band(0, 0, HIZ_DEPTH_TILE, &flat_shader);
	set_depth(XSDK_COMPARE_LESS, true, HIZ_ENABLE | HIZ_WRITE_ENABLE);
	draw_quad(&top, NEAR_DEPTH, NEAR_ARGB);
	if (!resolve_into(band_images[0], STEP_ROWS, "case 4 depth, band 0's near quad"))
		return false;
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t wrong_total = 0;
	for (uint32_t band = 0; band < STEPS; band++) {
		const uint32_t bits = band * step;
		const Rect rows = band_rect(band);
		set_band(band, 0, HIZ_DEPTH_TILE + band * band_tiles, &long_shader);
		xsdk_gpu_set_pixel_constants(0, zero, 1);
		set_depth(XSDK_COMPARE_LESS, false, HIZ_ENABLE | bits << HIZ_MOVE_SHIFT);
		xsdk_gpu_write_clock(xsdk_gpu_address(clock_words), true);
		for (uint32_t i = 0; i < HIDDEN_QUADS; i++)
			draw_quad(&rows, FAR_DEPTH, FAR_ARGB);
		xsdk_gpu_write_clock(xsdk_gpu_address(clock_words) + CLOCK_BYTES, true);
		if (!resolve_into(band_images[band], STEP_ROWS, "case 4 depth, the timed quads"))
			return false;
		const uint64_t clocks =
			xsdk_gpu_clock_value(clock_words + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clock_words);
		char first[TEXT_BYTES] = "";
		const uint32_t wrong = count_wrong(band, far_past_band_0, first, sizeof first);
		wrong_total += wrong;
		xsdk_log_write(
			"tiles probe: case 4 depth, hierarchical depth, bits 18-26 %s: band %u, depth at tile %u, the bits at %u: "
			"%u %s full-band quads took %u clocks, %u us; %u of %u pixels wrong%s",
			name, (unsigned)band, (unsigned)(HIZ_DEPTH_TILE + band * band_tiles), (unsigned)bits,
			(unsigned)HIDDEN_QUADS, band ? "drawn" : "hidden", (unsigned)clocks, (unsigned)clock_us(clocks),
			(unsigned)wrong, (unsigned)STEP_PIXELS, first);
	}
	xsdk_log_write("tiles probe: case 4 depth, hierarchical depth, bits 18-26 %s: %u pixels wrong in all", name,
		(unsigned)wrong_total);
	return true;
}

// Depth whole across bands, its base moved per band and wrapping past tile 2048, each pixel checked; then hidden
// quads timed with the hierarchical buffer's bits 18-26 not moved and moved by either of two guesses at their unit.
static bool run_depth(const ProbeCase *c)
{
	(void)c;
	const uint32_t band_tiles = xsdk_gpu_target_tiles(WIDTH, STEP_ROWS);
	const uint32_t pitch_pixels = xsdk_gpu_target_tiles(WIDTH, TILE_ROWS) * TILE_COLUMNS;
	xsdk_gpu_set_resolve_clears(true, false);
	const bool finished = depth_pass(0, band_tiles) && depth_pass(1, band_tiles) &&
		hiz_run("not moved", 0, band_tiles) && hiz_run("moved by the band's eDRAM tiles", band_tiles, band_tiles) &&
		hiz_run("moved by the band's pixels in 512s", STEP_ROWS * pitch_pixels / HIZ_PIXELS_A_STEP, band_tiles);
	xsdk_gpu_set_resolve_clears(true, true);
	return finished;
}

// A bin register's 64 bits in the case's form (xenia:src/xenia/gpu/command_processor.cc:793-828), _HI the opcode after
// _LO (xenos.h:1630-1633).
static void put_bins(uint32_t opcode, uint32_t word_opcode, uint64_t value)
{
	const uint32_t high = (uint32_t)(value >> 32);
	const uint32_t low = (uint32_t)value;
	if (bin_form == BINS_ONE_WORD) {
		volatile uint32_t *const packet = xsdk_gpu_reserve(4);
		packet[0] = xsdk_gpu_type3(word_opcode, 1);
		packet[1] = low;
		packet[2] = xsdk_gpu_type3(word_opcode + 1, 1);
		packet[3] = high;
		return;
	}
	const bool low_first = bin_form == BINS_LOW_FIRST;
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(opcode, 2);
	packet[1] = low_first ? low : high;
	packet[2] = low_first ? high : low;
}

static void put_mask(uint64_t value)
{
	put_bins(PM4_SET_BIN_MASK, PM4_SET_BIN_MASK_LO, value);
}

static void put_select(uint64_t value)
{
	put_bins(PM4_SET_BIN_SELECT, PM4_SET_BIN_SELECT_LO, value);
}

// xsdk_gpu_draw's packet with the predicate bit, unless this is the nopred break.
static void draw_predicated(void)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = xsdk_gpu_type3(PM4_DRAW_INDX_2, 1) | (broken("nopred") ? 0 : PREDICATE);
	packet[1] = XSDK_PRIMITIVE_TRIANGLE_LIST | AUTO_INDEX | QUAD_VERTICES << VERTEX_COUNT_SHIFT;
}

static void begin_bins(void)
{
	if (bin_form == BINS_RECORDED)
		xsdk_gpu_record_begin(&commands);
}

// For a recording, ends it and runs it from the ring; false if it outgrew its buffer, which xsdk logs.
static bool end_bins(void)
{
	if (bin_form != BINS_RECORDED)
		return true;
	if (!NT_SUCCESS(xsdk_gpu_record_end()))
		return false;
	xsdk_gpu_execute(&commands);
	return true;
}

// A row of predicated draws for each select, one for each mask, each into its own cell; both registers all ones after.
static void put_cells(const ProbeCase *c)
{
	set_state(WIDTH, HEIGHT, &flat_shader);
	for (uint32_t s = 0; s < c->select_count; s++) {
		put_select(c->selects[s]);
		for (uint32_t m = 0; m < MASKS; m++) {
			const Rect cell = cell_rect(s, m);
			put_mask(bin_masks[m]);
			place_quad(&cell, 0.0f, cell_argb(s, m));
			draw_predicated();
		}
	}
	put_mask(ALL_BINS);
	put_select(ALL_BINS);
}

static Outcome cell_outcome(uint32_t select, uint32_t mask)
{
	const Rect cell = cell_rect(select, mask);
	const uint32_t want = stored_of(cell_argb(select, mask));
	uint32_t drawn = 0;
	uint32_t clear = 0;
	for (uint32_t y = cell.y; y < cell.y + cell.height; y++) {
		for (uint32_t x = cell.x; x < cell.x + cell.width; x++) {
			const uint32_t stored = image[tiled_index(x, y, WIDTH)];
			drawn += stored == want;
			clear += stored == stored_of(SENTINEL_ARGB);
		}
	}
	return drawn == CELL_PIXELS ? LANDED : clear == CELL_PIXELS ? SKIPPED : PARTLY;
}

// One line a select: what each mask's draw did, against Xenia's model, which runs it if select & mask isn't 0.
static void log_select(const ProbeCase *c, uint32_t s)
{
	char line[LINE_BYTES] = "";
	bool same = true;
	for (uint32_t m = 0; m < MASKS; m++) {
		const Outcome got = cell_outcome(s, m);
		const Outcome model = c->selects[s] & bin_masks[m] ? LANDED : SKIPPED;
		same &= got == model;
		const size_t used = strlen(line);
		snprintf(line + used, sizeof line - used, " mask %08X_%08X %s (Xenia: %s);", (unsigned)(bin_masks[m] >> 32),
			(unsigned)bin_masks[m], outcome_names[got], outcome_names[model]);
	}
	xsdk_log_write("tiles probe: case %s, select %08X_%08X:%s %s", c->name, (unsigned)(c->selects[s] >> 32),
		(unsigned)c->selects[s], line, verdict(same));
}

// The bin packets alone, both all ones, then the predicated draws under each select; each step logged before it's
// queued, as either could hang the GPU.
static bool run_bins(const ProbeCase *c)
{
	bin_form = c->form;
	if (bin_form == BINS_RECORDED && !commands.dwords &&
		!NT_SUCCESS(xsdk_gpu_command_buffer_create(&commands, RECORD_DWORDS))) {
		xsdk_log_write("tiles probe: case %s: FAILED, the command buffer couldn't be made", c->name);
		return false;
	}
	xsdk_log_write("tiles probe: case %s: sending %s, both all ones, alone", c->name, bin_form_names[bin_form]);
	begin_bins();
	put_mask(ALL_BINS);
	put_select(ALL_BINS);
	if (!end_bins() || !finish("the bin packets alone"))
		return false;
	xsdk_log_write(
		"tiles probe: case %s: the bin packets ran; sending %u predicated draws under them, then both all "
		"ones again",
		c->name, (unsigned)(c->select_count * MASKS));
	begin_bins();
	put_cells(c);
	if (!end_bins() || !resolve_into(image, HEIGHT, "the predicated draws"))
		return false;
	for (uint32_t s = 0; s < c->select_count; s++)
		log_select(c, s);
	xsdk_log_write("tiles probe: case %s: both registers left all ones", c->name);
	return true;
}

// A predicated draw before any bin packet, and the same draw without the bit beside it, to show what XAM leaves set.
static bool run_bins_unset(const ProbeCase *c)
{
	xsdk_log_write("tiles probe: case %s: sending a predicated draw before any bin packet", c->name);
	const Rect predicated = cell_rect(0, 0);
	const Rect plain = cell_rect(0, 1);
	set_state(WIDTH, HEIGHT, &flat_shader);
	place_quad(&predicated, 0.0f, cell_argb(0, 0));
	draw_predicated();
	draw_quad(&plain, 0.0f, cell_argb(0, 1));
	if (!resolve_into(image, HEIGHT, "the draw before any bin packet"))
		return false;
	const Outcome got = cell_outcome(0, 0);
	const Outcome reference = cell_outcome(0, 1);
	xsdk_log_write(
		"tiles probe: case %s: the predicated draw %s, the one without the bit %s; Xenia starts both registers at "
		"0xFFFFFFFF, where it lands (xenia:src/xenia/gpu/command_processor.h:284-285); %s",
		c->name, outcome_names[got], outcome_names[reference], verdict(got == LANDED && reference == LANDED));
	return true;
}

// Case 11's draws, the same in a tiled pass's recording as untiled: full-screen quads through the long shader, draws
// of degenerate triangles that run only the vertex shader, or one pixel.
static void draw_cost_scene(CostScene scene)
{
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	place_width = COST_WIDTH;
	place_height = COST_HEIGHT;
	set_shaders(scene == SCENE_FILL ? &long_shader : &flat_shader);
	if (scene == SCENE_FILL) {
		xsdk_gpu_set_pixel_constants(0, zero, 1);
		for (uint32_t i = 0; i < FILL_QUADS; i++)
			draw_quad(&(Rect){ 0, 0, COST_WIDTH, COST_HEIGHT }, 0.0f, FILL_ARGB);
	} else if (scene == SCENE_VERTEX) {
		place_quad(&(Rect){ 0, 0, 1, 1 }, 0.0f, FILL_ARGB);
		xsdk_gpu_set_vertices(0, cost_vertices, VERTEX_BYTES);
		for (uint32_t i = 0; i < VERTEX_DRAWS; i++)
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, VERTEX_COUNT);
	} else {
		draw_quad(&(Rect){ 0, 0, 1, 1 }, 0.0f, FILL_ARGB);
	}
}

static uint32_t clock_span_us(void)
{
	return clock_us(xsdk_gpu_clock_value(clock_words + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clock_words));
}

// The samples the cost's tiled passes take.
static uint32_t cost_samples = 1;

// A 1280x720 tiled pass of the scene, band_rows a band, with depth, resolved into cost_frame.
static bool queue_tiled(CostScene scene, uint32_t band_rows)
{
	const XsdkTiling tiling = { .width = COST_WIDTH,
		.height = COST_HEIGHT,
		.samples = cost_samples,
		.targets = { { .format = XSDK_TARGET_8_8_8_8 } },
		.count = 1,
		.depth = true,
		.band_rows = band_rows };
	const XsdkTileResolve resolve = { .target = 0, .texture = &cost_frame };
	if (!NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &commands)))
		return false;
	draw_cost_scene(scene);
	return NT_SUCCESS(xsdk_gpu_end_tiles(&resolve, 1));
}

// The GPU's time for a frame of the scene, untiled or tiled, from before it to its resolves' end.
static bool time_frame(CostScene scene, uint32_t band_rows, uint32_t *us)
{
	for (uint32_t run = 0; run < COST_RUNS; run++) {
		xsdk_gpu_write_clock(xsdk_gpu_address(clock_words), true);
		if (band_rows) {
			if (!queue_tiled(scene, band_rows))
				return false;
		} else {
			xsdk_gpu_set_target(COST_WIDTH, COST_HEIGHT);
			draw_cost_scene(scene);
			xsdk_gpu_resolve_texture(&cost_frame, SENTINEL_ARGB);
		}
		xsdk_gpu_write_clock(xsdk_gpu_address(clock_words) + CLOCK_BYTES, true);
		if (!finish("case 11 cost"))
			return false;
	}
	*us = clock_span_us();
	return true;
}

static bool log_scene(const char *what, CostScene scene, uint32_t us[COST_TILINGS])
{
	for (uint32_t i = 0; i < COST_TILINGS; i++) {
		if (!time_frame(scene, cost_band_rows[i], &us[i]))
			return false;
	}
	xsdk_log_write("tiles probe: case 11 cost, %s: %s %u us, %s %u us, %s %u us, %s %u us", what, cost_tiling_names[0],
		(unsigned)us[0], cost_tiling_names[1], (unsigned)us[1], cost_tiling_names[2], (unsigned)us[2],
		cost_tiling_names[3], (unsigned)us[3]);
	return true;
}

// The vertex-bound draws in a pass of 2 bands, given band 0's rows or band 1's, timed in band 1, the last to run:
// skipped there by their predicate, or drawn.
static bool time_predicated(bool drawn, uint32_t *us)
{
	const XsdkTiling tiling = { .width = COST_WIDTH,
		.height = COST_HEIGHT,
		.samples = 1,
		.targets = { { .format = XSDK_TARGET_8_8_8_8 } },
		.count = 1,
		.depth = true,
		.band_rows = cost_band_rows[2] };
	const XsdkTileResolve resolve = { .target = 0, .texture = &cost_frame };
	const uint32_t band_rows = cost_band_rows[2];
	if (!NT_SUCCESS(xsdk_gpu_begin_tiles(&tiling, &commands)))
		return false;
	draw_cost_scene(SCENE_EMPTY);
	xsdk_gpu_set_vertices(0, cost_vertices, VERTEX_BYTES);
	xsdk_gpu_write_clock(xsdk_gpu_address(clock_words), true);
	xsdk_gpu_set_draw_rows(drawn ? band_rows : 0, drawn ? COST_HEIGHT : band_rows);
	for (uint32_t i = 0; i < VERTEX_DRAWS; i++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, VERTEX_COUNT);
	xsdk_gpu_set_draw_rows(0, COST_HEIGHT);
	xsdk_gpu_write_clock(xsdk_gpu_address(clock_words) + CLOCK_BYTES, true);
	if (!NT_SUCCESS(xsdk_gpu_end_tiles(&resolve, 1)) || !finish("case 11 cost, predicated"))
		return false;
	*us = clock_span_us();
	return true;
}

// The vertex buffer of degenerate triangles, every vertex at the origin, and the frame the passes resolve into.
static bool make_cost_resources(void)
{
	if (!commands.dwords && !NT_SUCCESS(xsdk_gpu_command_buffer_create(&commands, RECORD_DWORDS)))
		return false;
	volatile uint32_t *const vertices = xsdk_gpu_alloc(VERTEX_BYTES);
	if (!vertices ||
		!NT_SUCCESS(xsdk_texture_create(&cost_frame, XSDK_FORMAT_B8G8R8A8, COST_WIDTH, COST_HEIGHT, 1, true)))
		return false;
	const float origin[VERTEX_FLOATS] = { 0.0f, 0.0f, 0.0f, 1.0f };
	uint32_t words[VERTEX_FLOATS];
	memcpy(words, origin, sizeof words);
	for (uint32_t i = 0; i < VERTEX_BYTES / 4; i++)
		vertices[i] = words[i % VERTEX_FLOATS];
	__asm__ volatile("sync" ::: "memory");
	cost_vertices = xsdk_gpu_address(vertices);
	xsdk_gpu_invalidate(cost_vertices, VERTEX_BYTES);
	return true;
}

// A fill-bound and a vertex-bound 1280x720 frame untiled and in 1, 2 and 3 bands; a band's fixed cost from frames of
// one pixel; and a predicated draw outside its band against the same draw drawn, all by the GPU's clock.
static bool run_cost(const ProbeCase *c)
{
	(void)c;
	if (!make_cost_resources()) {
		xsdk_log_write("tiles probe: case 11 cost: FAILED, the buffers couldn't be made");
		return false;
	}
	uint32_t fill[COST_TILINGS];
	uint32_t vertex[COST_TILINGS];
	uint32_t empty[COST_TILINGS];
	uint32_t skipped = 0;
	uint32_t drawn = 0;
	if (!log_scene("fill-bound, 4 full-screen quads through the long shader", SCENE_FILL, fill) ||
		!log_scene("vertex-bound, 16 draws of 65532 degenerate triangles' vertices", SCENE_VERTEX, vertex) ||
		!log_scene("one pixel", SCENE_EMPTY, empty) || !time_predicated(false, &skipped) ||
		!time_predicated(true, &drawn))
		return false;
	for (uint32_t i = 0; i < sizeof cost_multisampled / sizeof cost_multisampled[0]; i++) {
		uint32_t fill_us = 0;
		uint32_t vertex_us = 0;
		cost_samples = cost_multisampled[i].samples;
		const bool timed = time_frame(SCENE_FILL, cost_multisampled[i].band_rows, &fill_us) &&
			time_frame(SCENE_VERTEX, cost_multisampled[i].band_rows, &vertex_us);
		cost_samples = 1;
		if (!timed)
			return false;
		xsdk_log_write(
			"tiles probe: case 11 cost, at %u samples in bands of %u rows: fill-bound %u us, vertex-bound %u us",
			(unsigned)cost_multisampled[i].samples, (unsigned)cost_multisampled[i].band_rows, (unsigned)fill_us,
			(unsigned)vertex_us);
	}
	// Each pixel is copied once whatever the bands, so what a band adds past one band is its fixed cost.
	const uint32_t fixed = empty[3] > empty[1] ? (empty[3] - empty[1]) / 2 : 0;
	xsdk_log_write("tiles probe: case 11 cost, a band's fixed cost: (%u - %u) / 2 = %u us; under %u us: %s",
		(unsigned)empty[3], (unsigned)empty[1], (unsigned)fixed, (unsigned)FIXED_COST_BAR_US,
		verdict(fixed < FIXED_COST_BAR_US));
	xsdk_log_write(
		"tiles probe: case 11 cost, the vertex-bound draws in band 1 of 2: predicated outside it %u us, drawn %u "
		"us; under a fifth: %s",
		(unsigned)skipped, (unsigned)drawn, verdict(skipped * SKIPPED_SHARE < drawn));
	xsdk_texture_free(&cost_frame);
	return true;
}

static const ProbeCase cases[] = {
	{ .name = "offset",
		.what = "each row of a 256x128 target drawn under a window offset of (0,-64) and a 256x64 screen scissor",
		.run = run_offset },
	{ .name = "position",
		.what = "the pixel parameters' xy drawn as colour under the same window offset",
		.run = run_position },
	{ .name = "resolve",
		.what = "32-row bands resolved with RB_COPY_DEST_BASE moved to rows 32, 64 and 96",
		.run = run_resolve },
	{ .name = "depth",
		.what = "depth whole across four 32-row bands, its base moved per band, then hierarchical depth timed",
		.run = run_depth },
	{ .name = "bins_high_first",
		.what = "predicated draws under 0x50 and 0x51, high word first",
		.run = run_bins,
		.form = BINS_HIGH_FIRST,
		.selects = bin_selects,
		.select_count = SELECTS },
	{ .name = "bins_low_first",
		.what = "predicated draws under 0x50 and 0x51, low word first",
		.run = run_bins,
		.form = BINS_LOW_FIRST,
		.selects = bin_selects,
		.select_count = SELECTS },
	{ .name = "bins_one_word",
		.what = "predicated draws under 0x60 to 0x63",
		.run = run_bins,
		.form = BINS_ONE_WORD,
		.selects = bin_selects,
		.select_count = SELECTS },
	{ .name = "bins_recorded",
		.what = "predicated draws and 0x50 and 0x51 inside a recording run from the ring",
		.run = run_bins,
		.form = BINS_RECORDED,
		.selects = bin_selects,
		.select_count = SELECTS },
	{ .name = "bins_unset",
		.what = "a predicated draw before any bin packet",
		.run = run_bins_unset,
		.form = BINS_NONE },
	{ .name = "bins_exit",
		.what = "both bin registers set all ones, a predicated draw for each mask, then the title ends",
		.run = run_bins,
		.form = BINS_HIGH_FIRST,
		.selects = &ALL_BINS,
		.select_count = 1 },
	{ .name = "cost",
		.what = "xsdk's tiled passes timed by the GPU's clock at 1280x720 in 1, 2 and 3 bands",
		.run = run_cost },
};

enum { CASE_COUNT = sizeof cases / sizeof cases[0] };

// The case game:\tiles_case.txt names by number or name, as an index; the first without the file or a case it names.
static uint32_t start_case(void)
{
	FILE *const file = fopen(start_path, "r");
	if (!file)
		return 0;
	char name[CASE_NAME_BYTES] = "";
	const bool read = fscanf(file, "%31s", name) == 1;
	(void)fclose(file);
	char *end = name;
	const unsigned long number = strtoul(name, &end, 10);
	const bool numbered = end != name && *end == '\0';
	for (uint32_t i = 0; read && i < CASE_COUNT; i++) {
		if (strcmp(name, cases[i].name) == 0 || (numbered && number == i + 1))
			return i;
	}
	xsdk_log_write("tiles probe: %s doesn't name a case; running cases 1 to %u and %u", start_path,
		(unsigned)SAFE_CASES, (unsigned)(COST_CASE + 1));
	return 0;
}

static bool load_shaders(void)
{
	return NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_load(&flat_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&position_shader, position_pixel_shader, sizeof position_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&long_shader, long_pixel_shader, sizeof long_pixel_shader));
}

// The shaders, the unit square and the images; false, logged, on a failure.
static bool make_resources(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(SQUARE_BYTES);
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, TEXTURE_ROWS));
	clock_words = xsdk_gpu_alloc(2 * CLOCK_BYTES);
	bool made = vertices && image && clock_words;
	for (uint32_t band = 0; band < STEPS; band++) {
		band_images[band] = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, STEP_ROWS));
		made &= band_images[band] != NULL;
	}
	if (!made || !load_shaders()) {
		xsdk_log_write("tiles probe: FAILED, the memory or the shaders couldn't be made");
		return false;
	}
	uint32_t words[SQUARE_BYTES / 4];
	memcpy(words, square, sizeof words);
	for (uint32_t i = 0; i < SQUARE_BYTES / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	square_address = xsdk_gpu_address(vertices);
	xsdk_gpu_invalidate(square_address, SQUARE_BYTES);
	return true;
}

int main(void)
{
	const uint32_t first = start_case();
	const uint32_t last = first < SAFE_CASES ? SAFE_CASES - 1 : first;
	xsdk_log_write("tiles probe: cases %u to %u%s (%s names the case to start from); break build: %s",
		(unsigned)(first + 1), (unsigned)(last + 1), first < SAFE_CASES ? " and 11" : "", start_path,
		broken("") ? "none" : TILES_BREAK);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("tiles probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources())
		return 1;
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	if (!resolve_into(image, HEIGHT, "the first clear"))
		return 1;
	for (uint32_t n = first; n <= last + (first < SAFE_CASES); n++) {
		// Past the safe cases, the sweep goes on to the cost case.
		const uint32_t i = n > last ? COST_CASE : n;
		const ProbeCase *const c = &cases[i];
		xsdk_log_write("tiles probe: case %u %s: %s", (unsigned)(i + 1), c->name, c->what);
		if (!c->run(c)) {
			// The GPU may be wedged: no more GPU work, and the exit's xsdk_gpu_close may only time out.
			xsdk_log_write("tiles probe: FAILED, case %u %s stopped; %u PASS, %u DIFFERS before it", (unsigned)(i + 1),
				c->name, (unsigned)passed, (unsigned)differed);
			return 1;
		}
	}
	status = xsdk_gpu_close();
	xsdk_log_write("tiles probe: done, %u PASS, %u DIFFERS; xsdk_gpu_close 0x%08X", (unsigned)passed,
		(unsigned)differed, (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
