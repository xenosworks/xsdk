#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "tests.h"
#include "text_font.h"
#include "tiled_index.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/text.h"
#include "xsdk/texture.h"

// Text on screen (milestone 57): each check draws a 1280x720 frame, resolves it into a texture and reads its pixels
// back. The glyphs are compared with the font's bits from src/text_font.h; blending, the scissor and the ring are
// checked on runs of the full block, inside them and around them.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	BLACK_ARGB = 0xFF000000,
	WHITE_ARGB = 0xFFFFFFFF,
	BLACK = 0x000000,
	WHITE = 0xFFFFFF,
	LEFT_BIT = 0x8000,
	// UTF-8 takes up to 3 bytes for the font's code points.
	UTF8_MOST = 3,
	FULL_BLOCK = 0x2588,
	// text_glyphs: the font's glyphs in lines below the line of samples.
	GLYPHS_X = 37,
	GLYPHS_Y = 23,
	SET_Y = 53,
	SET_LINE = (WIDTH - GLYPHS_X) / XSDK_TEXT_CELL_WIDTH,
	SCALE_3_Y = 83,
	// text_blend: a run of full blocks over mid grey, checked inside a pixel in from its edges and from 2 to 4 pixels
	// out from them.
	GREY_ARGB = 0xFF808080,
	GREY = 0x808080,
	BLEND_ARGB = 0x80FF0000,
	BLEND_TOLERANCE = 1,
	RUN_X = 200,
	RUN_Y = 200,
	RUN_CELLS = 16,
	AROUND_NEAR = 2,
	AROUND_FAR = 5,
	// text_layout's places.
	LAYOUT_X = 100,
	LAYOUT_Y = 100,
	DEFAULT_Y = 200,
	MALFORMED_X = 130,
	// text_scissor: the scissor, and a run of 20 blocks from inside it to past its right and bottom edges.
	SCISSOR_X = 300,
	SCISSOR_Y = 300,
	SCISSOR_WIDTH = 200,
	SCISSOR_HEIGHT = 100,
	CLIPPED_X = 400,
	CLIPPED_Y = 390,
	CLIPPED_CELLS = 20,
	// text_ring: 200 strings of 100 blocks a frame, every 5th of the first 180 on a line of its own, the rest above the
	// viewport; then the mark. The GPU waits at each frame's start until a thread lets it go, so a block the CPU wrote
	// again before the GPU drew it shows.
	RING_FRAMES = 3,
	RING_STRINGS = 200,
	RING_STRING_CELLS = 100,
	RING_EVERY = 5,
	RING_LINES = HEIGHT / XSDK_TEXT_CELL_HEIGHT,
	OFF_SCREEN_Y = -100,
	MARK_X = 1100,
	MARK_Y = 340,
	MARK_CELLS = 5,
	HOLD_MS = 50,
	// text_cost: a screen of glyphs at scale 1.
	COST_COLUMNS = WIDTH / XSDK_TEXT_CELL_WIDTH,
	COST_LINES = HEIGHT / XSDK_TEXT_CELL_HEIGHT,
	COST_MOST_US = 1000,
	PRINTABLE_FIRST = 0x21,
	PRINTABLE_COUNT = 94,
	GPU_CLOCKS_PER_MS = 498750,
};

// One of each block of the set: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, general punctuation, currency,
// arrows, box drawing, block elements and geometric shapes.
static const uint32_t samples[] = { 'A', 'z', '0', '9', 0xE9, 0xFF, 0x141, 0x178, 0x3A9, 0x3BB, 0x416, 0x44F, 0x2014,
	0x2030, 0x20AC, 0x2192, 0x21D2, 0x253C, 0x256C, 0x2580, 0x2593, 0x25C6, 0x25CB };

enum { SAMPLES = sizeof samples / sizeof samples[0] };

static XsdkTexture frame;
static uint32_t misses_logged;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void sleep_ms(uint32_t ms)
{
	const struct timespec request = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000 };
	(void)nanosleep(&request, NULL);
}

// The frame's pixel as 0xRRGGBB: an R8G8B8A8 texel's word holds red, green, blue, then alpha.
static uint32_t rgb_at(uint32_t x, uint32_t y)
{
	return frame.texels[tiled_index(x, y, frame.pitch)] >> 8;
}

static uint32_t glyph_of(uint32_t point)
{
	for (uint32_t i = 0; i < TEXT_FONT_GLYPHS; i++) {
		if (text_font_code_points[i] == point)
			return i;
	}
	return 0;
}

static uint32_t put_utf8(char *out, uint32_t point)
{
	if (point < 0x80) {
		out[0] = (char)point;
		return 1;
	}
	if (point < 0x800) {
		out[0] = (char)(0xC0 | point >> 6);
		out[1] = (char)(0x80 | (point & 0x3F));
		return 2;
	}
	out[0] = (char)(0xE0 | point >> 12);
	out[1] = (char)(0x80 | (point >> 6 & 0x3F));
	out[2] = (char)(0x80 | (point & 0x3F));
	return 3;
}

// The code points as a string in out, which holds UTF8_MOST bytes a code point and a NUL.
static void encode(char *out, const uint32_t *points, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		out += put_utf8(out, points[i]);
	*out = 0;
}

// eDRAM cleared to the background, then the target set for the check's draws.
static void begin_frame(uint32_t background_argb)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_resolve_texture(&frame, background_argb);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
}

// The frame resolved into the texture, once the GPU has drawn it.
static bool end_frame(void)
{
	xsdk_gpu_resolve_texture(&frame, BLACK_ARGB);
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

static bool near(uint32_t got, uint32_t want, uint32_t tolerance)
{
	for (uint32_t shift = 0; shift < 24; shift += 8) {
		const uint32_t a = got >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > tolerance)
			return false;
	}
	return true;
}

static bool pixel_right(const char *what, uint32_t x, uint32_t y, uint32_t want, uint32_t tolerance)
{
	const uint32_t got = rgb_at(x, y);
	if (near(got, want, tolerance))
		return true;
	if (misses_logged++ < MAX_MISSES_LOGGED)
		xsdk_log_write(
			"text: %s: (%u, %u) held %06X, not %06X", what, (unsigned)x, (unsigned)y, (unsigned)got, (unsigned)want);
	return false;
}

// Wrong pixels from (x0, y0) up to (x1, y1), each to be want.
static uint32_t wrong_rect(
	const char *what, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t want, uint32_t tolerance)
{
	uint32_t wrong = 0;
	for (uint32_t y = y0; y < y1; y++) {
		for (uint32_t x = x0; x < x1; x++)
			wrong += !pixel_right(what, x, y, want, tolerance);
	}
	return wrong;
}

// Wrong pixels in the cell drawn white on black from (x, y) at scale: lit where the glyph's bit is set.
static uint32_t wrong_cell(const char *what, uint32_t x, uint32_t y, uint32_t scale, uint32_t glyph)
{
	uint32_t wrong = 0;
	for (uint32_t row = 0; row < XSDK_TEXT_CELL_HEIGHT * scale; row++) {
		const uint32_t bits = text_font_rows[glyph][row / scale];
		for (uint32_t column = 0; column < XSDK_TEXT_CELL_WIDTH * scale; column++) {
			const uint32_t want = bits << column / scale & LEFT_BIT ? WHITE : BLACK;
			wrong += !pixel_right(what, x + column, y + row, want, 0);
		}
	}
	return wrong;
}

// Draws the code points white from (x, y) at scale.
static void draw_line(uint32_t x, uint32_t y, uint32_t scale, const uint32_t *points, uint32_t count)
{
	char text[SET_LINE * UTF8_MOST + 1];
	encode(text, points, count);
	xsdk_text_draw((int32_t)x, (int32_t)y, scale, WHITE_ARGB, text);
}

static uint32_t wrong_line(
	const char *what, uint32_t x, uint32_t y, uint32_t scale, const uint32_t *points, uint32_t count)
{
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < count; i++)
		wrong += wrong_cell(what, x + i * XSDK_TEXT_CELL_WIDTH * scale, y, scale, glyph_of(points[i]));
	return wrong;
}

// The samples, then every glyph of the set in lines below them, at scale 1.
static void check_glyphs(void)
{
	static uint32_t set[TEXT_FONT_GLYPHS];
	const uint32_t count = TEXT_FONT_GLYPHS - 1;
	for (uint32_t i = 0; i < count; i++)
		set[i] = text_font_code_points[i + 1];
	begin_frame(BLACK_ARGB);
	draw_line(GLYPHS_X, GLYPHS_Y, 1, samples, SAMPLES);
	for (uint32_t i = 0; i < count; i += SET_LINE)
		draw_line(GLYPHS_X, SET_Y + i / SET_LINE * XSDK_TEXT_CELL_HEIGHT, 1, &set[i],
			count - i < SET_LINE ? count - i : SET_LINE);
	const bool drawn = end_frame();
	misses_logged = 0;
	uint32_t wrong = drawn ? wrong_line("glyphs", GLYPHS_X, GLYPHS_Y, 1, samples, SAMPLES) : 1;
	for (uint32_t i = 0; drawn && i < count; i += SET_LINE)
		wrong += wrong_line("glyphs", GLYPHS_X, SET_Y + i / SET_LINE * XSDK_TEXT_CELL_HEIGHT, 1, &set[i],
			count - i < SET_LINE ? count - i : SET_LINE);
	xsdk_log_write(
		"text: %u glyphs and %u samples drawn, %u pixels wrong", (unsigned)count, (unsigned)SAMPLES, (unsigned)wrong);
	check(
		"text: every glyph of the font, and a line with one from each of its blocks at (37, 23), drawn at scale 1, "
		"each cell's pixels the font's bits",
		wrong == 0);
}

static void check_scale(void)
{
	begin_frame(BLACK_ARGB);
	draw_line(GLYPHS_X, GLYPHS_Y, 2, samples, SAMPLES);
	draw_line(GLYPHS_X, SCALE_3_Y, 3, samples, SAMPLES);
	const bool drawn = end_frame();
	misses_logged = 0;
	const uint32_t wrong_2 = drawn ? wrong_line("scale 2", GLYPHS_X, GLYPHS_Y, 2, samples, SAMPLES) : 1;
	const uint32_t wrong_3 = drawn ? wrong_line("scale 3", GLYPHS_X, SCALE_3_Y, 3, samples, SAMPLES) : 1;
	xsdk_log_write("text: the samples at scales 2 and 3, %u and %u pixels wrong", (unsigned)wrong_2, (unsigned)wrong_3);
	check("text: the line of samples at scales 2 and 3, each of the font's pixels a 2x2 and a 3x3 block",
		wrong_2 == 0 && wrong_3 == 0);
}

// A run of full blocks drawn from (x, y) at scale 1.
static void draw_run(uint32_t x, uint32_t y, uint32_t cells, uint32_t argb)
{
	char text[CLIPPED_CELLS * UTF8_MOST + 1];
	uint32_t points[CLIPPED_CELLS];
	for (uint32_t i = 0; i < cells; i++)
		points[i] = FULL_BLOCK;
	encode(text, points, cells);
	xsdk_text_draw((int32_t)x, (int32_t)y, 1, argb, text);
}

// A channel of argb over a channel of mid grey, at argb's alpha.
static uint32_t over_grey(uint32_t argb, uint32_t shift)
{
	const float alpha = (float)(argb >> 24) / 255.0f;
	const float source = (float)(argb >> shift & 0xFF);
	return (uint32_t)(source * alpha + (float)(GREY >> shift & 0xFF) * (1.0f - alpha) + 0.5f) << shift;
}

static void check_blend(void)
{
	begin_frame(GREY_ARGB);
	draw_run(RUN_X, RUN_Y, RUN_CELLS, BLEND_ARGB);
	const bool drawn = end_frame();
	misses_logged = 0;
	const uint32_t blended = over_grey(BLEND_ARGB, 16) | over_grey(BLEND_ARGB, 8) | over_grey(BLEND_ARGB, 0);
	const uint32_t right = RUN_X + RUN_CELLS * XSDK_TEXT_CELL_WIDTH;
	const uint32_t bottom = RUN_Y + XSDK_TEXT_CELL_HEIGHT;
	const uint32_t inside =
		drawn ? wrong_rect("blend", RUN_X + 1, RUN_Y + 1, right - 1, bottom - 1, blended, BLEND_TOLERANCE) : 1;
	// The band around the run, 2 to 4 pixels out.
	uint32_t around = wrong_rect("around", RUN_X - AROUND_FAR + 1, RUN_Y - AROUND_FAR + 1, right + AROUND_FAR - 1,
		RUN_Y - AROUND_NEAR + 1, GREY, 0);
	around += wrong_rect("around", RUN_X - AROUND_FAR + 1, bottom + AROUND_NEAR - 1, right + AROUND_FAR - 1,
		bottom + AROUND_FAR - 1, GREY, 0);
	around += wrong_rect("around", RUN_X - AROUND_FAR + 1, RUN_Y, RUN_X - AROUND_NEAR + 1, bottom, GREY, 0);
	around += wrong_rect("around", right + AROUND_NEAR - 1, RUN_Y, right + AROUND_FAR - 1, bottom, GREY, 0);
	xsdk_log_write("text: blended to %06X, %u pixels wrong inside the run, %u around it", (unsigned)blended,
		(unsigned)inside, (unsigned)around);
	check("text: 0x80FF0000 over mid grey, its pixels blended to within 1 a channel and the others untouched",
		drawn && inside == 0 && around == 0);
}

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t point;
} Cell;

// "a", e acute, a newline, then Zhe, a box drawing line and a full block; the cell after the first line stays empty.
static const char layout_text[] = "a\u00E9\n\u0416\u2500\u2588";
static const char first_line[] = "a\u00E9";
static const Cell layout_cells[] = {
	{ LAYOUT_X, LAYOUT_Y, 'a' },
	{ LAYOUT_X + 10, LAYOUT_Y, 0xE9 },
	{ LAYOUT_X + 20, LAYOUT_Y, ' ' },
	{ LAYOUT_X, LAYOUT_Y + 20, 0x416 },
	{ LAYOUT_X + 10, LAYOUT_Y + 20, 0x2500 },
	{ LAYOUT_X + 20, LAYOUT_Y + 20, FULL_BLOCK },
	{ LAYOUT_X + 30, LAYOUT_Y + 20, ' ' },
	// U+4E00, which the font's set lacks, and a lone 0xFF: the default box, glyph 0.
	{ LAYOUT_X, DEFAULT_Y, 0 },
	{ MALFORMED_X, DEFAULT_Y, 0 },
};

static void check_layout(void)
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t first_width = 0;
	uint32_t first_height = 0;
	xsdk_text_measure(layout_text, 1, &width, &height);
	xsdk_text_measure(first_line, 1, &first_width, &first_height);
	begin_frame(BLACK_ARGB);
	xsdk_text_draw(LAYOUT_X, LAYOUT_Y, 1, WHITE_ARGB, layout_text);
	xsdk_text_draw(LAYOUT_X, DEFAULT_Y, 1, WHITE_ARGB, "\u4E00");
	xsdk_text_draw(MALFORMED_X, DEFAULT_Y, 1, WHITE_ARGB, "\xFF");
	const bool drawn = end_frame();
	misses_logged = 0;
	uint32_t wrong = drawn ? 0 : 1;
	for (uint32_t i = 0; drawn && i < sizeof layout_cells / sizeof layout_cells[0]; i++) {
		const Cell *const cell = &layout_cells[i];
		wrong += wrong_cell("layout", cell->x, cell->y, 1, glyph_of(cell->point));
	}
	xsdk_log_write("text: measured %ux%u, the first line %ux%u; %u pixels wrong", (unsigned)width, (unsigned)height,
		(unsigned)first_width, (unsigned)first_height, (unsigned)wrong);
	check(
		"text: a two-line string measures 30x40 and its first line 20x20, its second line drawn at x 20 below, and "
		"U+4E00 and a lone 0xFF drawn as the default box",
		width == 30 && height == 40 && first_width == 20 && first_height == 20 && wrong == 0);
}

static void check_scissor(void)
{
	begin_frame(BLACK_ARGB);
	xsdk_gpu_set_scissor(SCISSOR_X, SCISSOR_Y, SCISSOR_WIDTH, SCISSOR_HEIGHT);
	draw_run(CLIPPED_X, CLIPPED_Y, CLIPPED_CELLS, WHITE_ARGB);
	const bool drawn = end_frame();
	misses_logged = 0;
	const uint32_t right = SCISSOR_X + SCISSOR_WIDTH;
	const uint32_t bottom = SCISSOR_Y + SCISSOR_HEIGHT;
	const uint32_t run_right = CLIPPED_X + CLIPPED_CELLS * XSDK_TEXT_CELL_WIDTH;
	const uint32_t run_bottom = CLIPPED_Y + XSDK_TEXT_CELL_HEIGHT;
	const uint32_t inside =
		drawn ? wrong_rect("inside the scissor", CLIPPED_X + 1, CLIPPED_Y + 1, right, bottom, WHITE, 0) : 1;
	// Past the scissor's right edge and below its bottom, to a few pixels past the run.
	uint32_t outside = wrong_rect("outside the scissor", right, CLIPPED_Y - AROUND_FAR, run_right + AROUND_FAR,
		run_bottom + AROUND_FAR, BLACK, 0);
	outside +=
		wrong_rect("outside the scissor", CLIPPED_X - AROUND_FAR, bottom, right, run_bottom + AROUND_FAR, BLACK, 0);
	xsdk_log_write("text: %u pixels wrong inside the scissor, %u outside it", (unsigned)inside, (unsigned)outside);
	check("text: a string half outside the scissor leaves the pixels outside it untouched",
		drawn && inside == 0 && outside == 0);
}

typedef struct {
	volatile uint32_t *word;
	uint32_t value;
} Release;

// Lets the GPU past the word it waits on, once the drawing thread has had time to queue all it can.
static void *release_gpu(void *context)
{
	const Release *const release = context;
	sleep_ms(HOLD_MS);
	*release->word = release->value;
	__asm__ volatile("sync" ::: "memory");
	return NULL;
}

// Wrong pixels inside the cells of a run drawn from (x, y): a pixel in from each edge of its first and last.
static uint32_t wrong_ends(uint32_t x, uint32_t y, uint32_t cells)
{
	const uint32_t last = x + (cells - 1) * XSDK_TEXT_CELL_WIDTH;
	return wrong_rect("ring", x + 1, y + 1, x + XSDK_TEXT_CELL_WIDTH - 1, y + XSDK_TEXT_CELL_HEIGHT - 1, WHITE, 0) +
		wrong_rect("ring", last + 1, y + 1, last + XSDK_TEXT_CELL_WIDTH - 1, y + XSDK_TEXT_CELL_HEIGHT - 1, WHITE, 0);
}

static bool on_screen(uint32_t string)
{
	return string % RING_EVERY == 0 && string / RING_EVERY < RING_LINES;
}

// One frame of the ring's check, the GPU held at its start until value is written to word.
static uint32_t ring_frame(volatile uint32_t *word, uint32_t value, const char *run, bool *drawn)
{
	char mark[MARK_CELLS * UTF8_MOST + 1];
	const uint32_t blocks[MARK_CELLS] = { FULL_BLOCK, FULL_BLOCK, FULL_BLOCK, FULL_BLOCK, FULL_BLOCK };
	encode(mark, blocks, MARK_CELLS);
	begin_frame(BLACK_ARGB);
	xsdk_gpu_wait_word(xsdk_gpu_address(word), value);
	Release release = { .word = word, .value = value };
	pthread_t thread;
	const bool started = pthread_create(&thread, NULL, release_gpu, &release) == 0;
	if (!started)
		(void)release_gpu(&release);
	for (uint32_t i = 0; i < RING_STRINGS; i++) {
		const int32_t y = on_screen(i) ? (int32_t)(i / RING_EVERY * XSDK_TEXT_CELL_HEIGHT) : OFF_SCREEN_Y;
		xsdk_text_draw(0, y, 1, WHITE_ARGB, run);
	}
	xsdk_text_draw(MARK_X, MARK_Y, 1, WHITE_ARGB, mark);
	if (started)
		(void)pthread_join(thread, NULL);
	*drawn = end_frame() && started;
	if (!*drawn)
		return 1;
	uint32_t wrong = wrong_ends(MARK_X, MARK_Y, MARK_CELLS);
	for (uint32_t i = 0; i < RING_STRINGS; i++) {
		if (on_screen(i))
			wrong += wrong_ends(0, i / RING_EVERY * XSDK_TEXT_CELL_HEIGHT, RING_STRING_CELLS);
	}
	return wrong;
}

static void check_ring(volatile uint32_t *word)
{
	static char run[RING_STRING_CELLS * UTF8_MOST + 1];
	static uint32_t blocks[RING_STRING_CELLS];
	for (uint32_t i = 0; i < RING_STRING_CELLS; i++)
		blocks[i] = FULL_BLOCK;
	encode(run, blocks, RING_STRING_CELLS);
	*word = 0;
	__asm__ volatile("sync" ::: "memory");
	misses_logged = 0;
	uint32_t wrong = 0;
	bool drawn = true;
	const uint64_t start = now_us();
	for (uint32_t i = 0; i < RING_FRAMES; i++) {
		bool frame_drawn = false;
		wrong += ring_frame(word, i + 1, run, &frame_drawn);
		drawn = drawn && frame_drawn;
	}
	xsdk_log_write("text: %u frames of %u glyphs and a mark in %llu us, %u pixels wrong", (unsigned)RING_FRAMES,
		(unsigned)(RING_STRINGS * RING_STRING_CELLS), (unsigned long long)(now_us() - start), (unsigned)wrong);
	check(
		"text: 20,000 glyphs a frame, more than the ring holds, then a marked string, over three frames, the GPU held "
		"at each frame's start: every string on screen has its first and last glyph where drawn",
		drawn && wrong == 0);
}

// A screen of printable ASCII, a draw a line; the GPU's clock read before and after, once it's idle.
static void check_cost(volatile uint32_t *clocks)
{
	static char lines[COST_LINES][COST_COLUMNS + 1];
	for (uint32_t i = 0; i < COST_LINES * COST_COLUMNS; i++)
		lines[i / COST_COLUMNS][i % COST_COLUMNS] = (char)(PRINTABLE_FIRST + i % PRINTABLE_COUNT);
	begin_frame(BLACK_ARGB);
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks), true);
	const uint64_t start = now_us();
	for (uint32_t i = 0; i < COST_LINES; i++)
		xsdk_text_draw(0, (int32_t)(i * XSDK_TEXT_CELL_HEIGHT), 1, WHITE_ARGB, lines[i]);
	const uint64_t queued_us = now_us() - start;
	xsdk_gpu_write_clock(xsdk_gpu_address(clocks + XSDK_GPU_CLOCK_WORDS), true);
	const bool drawn = end_frame();
	const uint64_t gpu_clocks = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clocks);
	xsdk_log_write("text: %u glyphs queued in %llu us of CPU, drawn in %llu us of GPU",
		(unsigned)(COST_LINES * COST_COLUMNS), (unsigned long long)queued_us,
		(unsigned long long)(gpu_clocks * 1000 / GPU_CLOCKS_PER_MS));
	check("text: a 1280x720 screen of glyphs, 4,608, queued in under 1 ms of CPU", drawn && queued_us < COST_MOST_US);
}

void test_text(void)
{
	NTSTATUS status = xsdk_gpu_open();
	check("text: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	status = xsdk_text_open();
	if (!NT_SUCCESS(status))
		xsdk_log_write("text: xsdk_text_open failed: 0x%08X", (unsigned)status);
	volatile uint32_t *const words = xsdk_gpu_alloc((1 + 2 * XSDK_GPU_CLOCK_WORDS) * sizeof(uint32_t));
	const bool made = NT_SUCCESS(status) && words &&
		NT_SUCCESS(xsdk_texture_create(&frame, XSDK_FORMAT_R8G8B8A8, WIDTH, HEIGHT, 1, true));
	check("text: the text open, and a frame to read back", made);
	if (made) {
		check_glyphs();
		check_scale();
		check_blend();
		check_layout();
		check_scissor();
		check_ring(words);
		check_cost(words + 1);
	}
	xsdk_texture_free(&frame);
	xsdk_gpu_free(words);
	xsdk_text_close();
	check("text: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
