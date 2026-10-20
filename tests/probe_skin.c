#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe_skin_shaders.h"
#include "tiled_index.h"
#include "xsc_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/xsc.h"

// Why xsdk_test's skinning check passed on the console with skin.patch's old break, whose container the simulator
// draws away from the quad (results/2026-10-20-milestone-50/README.md): the skinning cell drawn as tests/test_hlsl.c
// draws it, from each case's container. This title's xost is that break's, tests/probe_skin.patch's xsc_lower_block.c
// linked ahead of libxsdk.a (Makefile, skin_probe). game:\skin_start.txt names one case; without it every case runs.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FRAMES = 3,
	FENCE_TIMEOUT_MS = 1000,
	RAMP_TOLERANCE = 2,
	// tests/test_hlsl.c's skinning cell, its quad and vertices: POSITION, BLENDWEIGHT, UBYTE4 BLENDINDICES, TEXCOORD0.
	CELL_WIDTH = 80,
	CELL_HEIGHT = 40,
	SKIN_X = 860,
	SKIN_Y = 645,
	SKIN_MARGIN = 8,
	SKIN_QUAD_X = SKIN_X + SKIN_MARGIN,
	SKIN_QUAD_Y = SKIN_Y + SKIN_MARGIN,
	SKIN_QUAD_WIDTH = CELL_WIDTH - 2 * SKIN_MARGIN,
	SKIN_QUAD_HEIGHT = CELL_HEIGHT - 2 * SKIN_MARGIN,
	QUAD_VERTICES = 6,
	SKIN_VERTEX_WORDS = 11,
	SKIN_STRIDE_BYTES = SKIN_VERTEX_WORDS * 4,
	SKIN_INDICES_WORD = 8,
	BONES = 8,
	BONE_ROWS = 3,
	BONE_REGISTERS = BONES * BONE_ROWS,
	BONES_A_VERTEX = 4,
	NAME_BYTES = 16,
};

// Outside the enum: a value past INT_MAX there makes GCC give every constant in it unsigned int, -CELL_HEIGHT too.
static const uint32_t CLEAR_ARGB = 0xFF202020;

typedef enum {
	SOURCE_PC,
	SOURCE_LANEX,
	SOURCE_TITLE,
} Source;

typedef struct {
	const char *name;
	const char *what;
	Source source;
	// Bones 0 and 1 swapped in the constants, the CPU's skinning left as it is.
	bool swapped;
} SkinCase;

// What the simulator draws for each is in results/2026-10-20-milestone-50/README.md.
static const SkinCase cases[] = {
	{ "pc", "the PC's container, which xsdk_test draws", SOURCE_PC, false },
	{ "lanex", "the PC's container from tests/probe_skin.patch's compiler", SOURCE_LANEX, false },
	{ "title", "this title's own compile, by tests/probe_skin.patch's compiler, as x_hlsl_skin's", SOURCE_TITLE,
		false },
	{ "swapped", "the PC's container, bones 0 and 1 swapped in the constants", SOURCE_PC, true },
};

enum { CASES = sizeof cases / sizeof cases[0] };

static const char *const start_path = "game:\\skin_start.txt";

static const XsdkVertexElement skin_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 },
	{ .usage = XSDK_USAGE_BLENDWEIGHT, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	{ .usage = XSDK_USAGE_BLENDINDICES, .format = XSDK_VERTEX_UBYTE4, .offset_bytes = SKIN_INDICES_WORD * 4 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 36 },
};

static const uint32_t strides[] = { SKIN_STRIDE_BYTES };
static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
static const uint8_t corner_bones[4][BONES_A_VERTEX] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 6, 7 },
	{ 4, 5, 2, 3 } };
static const float skin_weights[BONES_A_VERTEX] = { 0.5f, 0.25f, 0.125f, 0.125f };

static float bones[BONE_REGISTERS][4];
static float swapped_bones[BONE_REGISTERS][4];
static float skinned[4][BONE_ROWS];
static XsdkRasterState raster;
static volatile uint32_t *vertices;
static volatile uint32_t *image;

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

// tests/test_hlsl.c's palette: each bone the whole skinning plus its pair's change, each corner's changes cancelling.
static void make_bones(void)
{
	const float whole[BONE_ROWS][4] = {
		{ SKIN_QUAD_WIDTH / (WIDTH / 2.0f), 0.0f, 0.0f, (SKIN_QUAD_X - 0.5f) / (WIDTH / 2.0f) - 1.0f },
		{ 0.0f, -SKIN_QUAD_HEIGHT / (HEIGHT / 2.0f), 0.0f, 1.0f - (SKIN_QUAD_Y - 0.5f) / (HEIGHT / 2.0f) },
		{ 0.5f, 0.0f, 0.0f, 0.25f },
	};
	static const struct {
		uint32_t row;
		uint32_t column;
		float by;
	} changes[BONES / 2] = { { 0, 3, 0.25f }, { 1, 3, 0.25f }, { 0, 0, 0.125f }, { 2, 0, 0.25f } };
	static const float times[4] = { 1.0f, -2.0f, 1.0f, -1.0f };
	for (uint32_t b = 0; b < BONES; b++) {
		memcpy(bones[b * BONE_ROWS], whole, sizeof whole);
		bones[b * BONE_ROWS + changes[b / 2].row][changes[b / 2].column] += times[b % 4] * changes[b / 2].by;
	}
	memcpy(swapped_bones, bones, sizeof bones);
	memcpy(swapped_bones[0], bones[BONE_ROWS], sizeof bones[0] * BONE_ROWS);
	memcpy(swapped_bones[BONE_ROWS], bones[0], sizeof bones[0] * BONE_ROWS);
	for (uint32_t corner = 0; corner < 4; corner++) {
		const float pos[4] = { (float)(corner & 1), (float)(corner >> 1), 0.0f, 1.0f };
		for (uint32_t j = 0; j < BONE_ROWS; j++) {
			skinned[corner][j] = 0.0f;
			for (uint32_t k = 0; k < BONES_A_VERTEX; k++) {
				const float *const row = bones[corner_bones[corner][k] * BONE_ROWS + j];
				skinned[corner][j] +=
					skin_weights[k] * (row[0] * pos[0] + row[1] * pos[1] + row[2] * pos[2] + row[3] * pos[3]);
			}
		}
	}
}

static void put_skin_vertices(void)
{
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float u = corners[v][0], w = corners[v][1];
		const uint8_t *const b = corner_bones[(uint32_t)u + 2 * (uint32_t)w];
		const float floats[SKIN_INDICES_WORD] = { u, w, 0.0f, 1.0f, skin_weights[0], skin_weights[1], skin_weights[2],
			0.0f };
		volatile uint32_t *const vertex = vertices + v * SKIN_VERTEX_WORDS;
		for (uint32_t k = 0; k < SKIN_INDICES_WORD; k++)
			vertex[k] = float_bits(floats[k]);
		vertex[SKIN_INDICES_WORD] = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
		vertex[SKIN_INDICES_WORD + 1] = float_bits(u);
		vertex[SKIN_INDICES_WORD + 2] = 0;
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), QUAD_VERTICES * SKIN_STRIDE_BYTES);
}

static uint32_t stored(uint32_t r, uint32_t g, uint32_t b)
{
	return b << 24 | g << 16 | r << 8 | 0xFF;
}

static uint32_t unorm(float value)
{
	return (uint32_t)(value * 255.0f + 0.5f);
}

static bool close_to(uint32_t held, uint32_t want)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t a = held >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > RAMP_TOLERANCE)
			return false;
	}
	return true;
}

// tests/test_hlsl.c's skin_colour: the quad where the palette puts it, red its skinned z, green 0.75, blue TEXCOORD0's
// x; the clear around it.
static uint32_t skin_colour(uint32_t x, uint32_t y)
{
	if (x < SKIN_QUAD_X || x >= SKIN_QUAD_X + SKIN_QUAD_WIDTH || y < SKIN_QUAD_Y || y >= SKIN_QUAD_Y + SKIN_QUAD_HEIGHT)
		return stored(CLEAR_ARGB >> 16 & 0xFF, CLEAR_ARGB >> 8 & 0xFF, CLEAR_ARGB & 0xFF);
	const float u = ((float)(x - SKIN_QUAD_X) + 0.5f) / SKIN_QUAD_WIDTH;
	return stored(unorm(skinned[0][2] + (skinned[1][2] - skinned[0][2]) * u), unorm(0.75f), unorm(u));
}

static void set_literals(const XsdkXscNeed *needs, size_t count, bool pixel)
{
	for (size_t i = 0; i < count; i++) {
		const float value[4] = { (float)needs[i].value[0], (float)needs[i].value[1], (float)needs[i].value[2],
			(float)needs[i].value[3] };
		if (needs[i].kind != XSDK_XSC_LITERAL)
			continue;
		if (pixel)
			xsdk_gpu_set_pixel_constants(needs[i].reg, value, 1);
		else
			xsdk_gpu_set_vertex_constants(needs[i].reg, value, 1);
	}
}

// The title's compile set against the old break's PC container: the same, or the first dword that differs.
static void compare_title(const XsdkXscShader *title)
{
	const uint32_t *const got = (const uint32_t *)title->container;
	const size_t words = title->container_bytes / 4, want = sizeof probe_skin_lanex / 4;
	size_t i = 0;
	while (i < words && i < want && got[i] == probe_skin_lanex[i])
		i++;
	if (i == words && i == want) {
		xsdk_log_write("skin probe: the title's compile is the lanex case's container, %u bytes",
			(unsigned)title->container_bytes);
		return;
	}
	xsdk_log_write(
		"skin probe: the title's compile, %u bytes, differs from the lanex case's, %u, at dword %u: "
		"0x%08X for 0x%08X",
		(unsigned)title->container_bytes, (unsigned)(want * 4), (unsigned)i, (unsigned)(i < words ? got[i] : 0),
		(unsigned)(i < want ? probe_skin_lanex[i] : 0));
}

// Draws FRAMES frames; counts the cell's pixels unlike the CPU's skinning after the first, and finds the box of every
// pixel not the clear colour in the last. False if a fence didn't land.
static bool draw_frames(const SkinCase *c, const XsdkShader *vertex, const XsdkShader *pixel, const XsdkXscNeed *needs,
	size_t need_count, const XscTestShader *pixel_source, uint32_t *wrong, uint32_t box[4])
{
	*wrong = 0;
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_raster(&raster);
		xsdk_gpu_set_shaders(vertex, pixel);
		set_literals(needs, need_count, false);
		set_literals(pixel_source->needs, pixel_source->need_count, true);
		xsdk_gpu_set_vertex_constants(0, c->swapped ? swapped_bones[0] : bones[0], BONE_REGISTERS);
		xsdk_gpu_set_vertices(0, xsdk_gpu_address(vertices), QUAD_VERTICES * SKIN_STRIDE_BYTES);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
		const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("skin probe: %s: the fence didn't land: 0x%08X", c->name, (unsigned)status);
			return false;
		}
		// The first frame's colour is whatever eDRAM held; its resolve clears it.
		for (uint32_t y = SKIN_Y; frame > 0 && y < SKIN_Y + CELL_HEIGHT; y++) {
			for (uint32_t x = SKIN_X; x < SKIN_X + CELL_WIDTH; x++)
				*wrong += !close_to(image[tiled_index(x, y, WIDTH)], skin_colour(x, y));
		}
	}
	const uint32_t clear = stored(CLEAR_ARGB >> 16 & 0xFF, CLEAR_ARGB >> 8 & 0xFF, CLEAR_ARGB & 0xFF);
	box[0] = WIDTH;
	box[1] = 0;
	box[2] = HEIGHT;
	box[3] = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			if (image[tiled_index(x, y, WIDTH)] == clear)
				continue;
			box[0] = x < box[0] ? x : box[0];
			box[1] = x > box[1] ? x : box[1];
			box[2] = y < box[2] ? y : box[2];
			box[3] = y > box[3] ? y : box[3];
		}
	}
	return true;
}

// False if a fence didn't land, after which the GPU is wedged.
static bool run_case(const SkinCase *c, const XscTestShader *vertex_source, const XscTestShader *pixel_source)
{
	XsdkXscShader title = { 0 };
	const uint32_t *container = vertex_source->container;
	uint32_t bytes = (uint32_t)(vertex_source->container_words * 4);
	const XsdkXscNeed *needs = vertex_source->needs;
	size_t need_count = vertex_source->need_count;
	if (c->source == SOURCE_LANEX) {
		container = probe_skin_lanex;
		bytes = sizeof probe_skin_lanex;
		needs = probe_skin_lanex_needs;
		need_count = sizeof probe_skin_lanex_needs / sizeof probe_skin_lanex_needs[0];
	} else if (c->source == SOURCE_TITLE) {
		XsdkXscOptions options;
		xsdk_xsc_default_options(&options);
		options.optimize = true;
		const XsdkXscStatus status =
			xsdk_xsc_compile(vertex_source->spirv, vertex_source->spirv_words, &options, &title);
		if (status != XSDK_XSC_OK) {
			xsdk_log_write("skin probe: %s: the title's compile failed: %d, %s", c->name, status, title.message);
			return true;
		}
		compare_title(&title);
		container = (const uint32_t *)title.container;
		bytes = (uint32_t)title.container_bytes;
		needs = title.needs;
		need_count = title.need_count;
	}
	XsdkShader vertex = { 0 }, pixel = { 0 };
	bool landed = true;
	if (!NT_SUCCESS(xsdk_shader_load(&vertex, container, bytes)) ||
		!NT_SUCCESS(
			xsdk_shader_set_vertex_layout(&vertex, skin_layout, sizeof skin_layout / sizeof skin_layout[0], strides)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel, pixel_source->container, (uint32_t)(pixel_source->container_words * 4))) ||
		!NT_SUCCESS(xsdk_shader_link(&vertex, &pixel))) {
		xsdk_log_write("skin probe: %s: its shaders didn't load or link; skipped", c->name);
	} else {
		uint32_t wrong, box[4];
		landed = draw_frames(c, &vertex, &pixel, needs, need_count, pixel_source, &wrong, box);
		if (landed && box[0] > box[1]) {
			xsdk_log_write("skin probe: %s, %s: %u of %u pixels of its cell wrong; nothing drawn", c->name, c->what,
				(unsigned)wrong, (unsigned)((FRAMES - 1) * CELL_WIDTH * CELL_HEIGHT));
		} else if (landed) {
			xsdk_log_write(
				"skin probe: %s, %s: %u of %u pixels of its cell wrong; drawn over columns %u to %u, rows %u "
				"to %u",
				c->name, c->what, (unsigned)wrong, (unsigned)((FRAMES - 1) * CELL_WIDTH * CELL_HEIGHT),
				(unsigned)box[0], (unsigned)box[1], (unsigned)box[2], (unsigned)box[3]);
		}
	}
	if (pixel.microcode)
		xsdk_shader_free(&pixel);
	if (vertex.microcode)
		xsdk_shader_free(&vertex);
	xsdk_xsc_free(&title);
	return landed;
}

// The case game:\skin_start.txt names, or NULL for every case.
static const SkinCase *chosen_case(void)
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
	return NULL;
}

int main(void)
{
	const XscTestShader *const vertex_source = xsc_shader("hlsl/grid_skin_vs.hlsl");
	const XscTestShader *const pixel_source = xsc_shader("hlsl/grid_link_ps.hlsl");
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("skin probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	vertices = xsdk_gpu_alloc(QUAD_VERTICES * SKIN_STRIDE_BYTES);
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!vertex_source || !pixel_source || !vertices || !image) {
		xsdk_log_write("skin probe: FAILED, the shaders or the buffers weren't there");
		return 1;
	}
	xsdk_gpu_raster_state(&raster, &(XsdkRaster){ .cull = XSDK_CULL_NONE, .front_clockwise = true });
	make_bones();
	put_skin_vertices();
	const SkinCase *const chosen = chosen_case();
	xsdk_log_write("skin probe: %s; the CPU's quad from columns %.1f to %.1f, rows %.1f to %.1f",
		chosen ? chosen->name : "every case, as game:\\skin_start.txt names none", (skinned[0][0] + 1.0f) * WIDTH / 2,
		(skinned[1][0] + 1.0f) * WIDTH / 2, (1.0f - skinned[0][1]) * HEIGHT / 2, (1.0f - skinned[2][1]) * HEIGHT / 2);
	for (uint32_t i = 0; i < CASES; i++) {
		if (chosen && chosen != &cases[i])
			continue;
		if (!run_case(&cases[i], vertex_source, pixel_source)) {
			xsdk_log_write("skin probe: FAILED, the GPU didn't finish %s", cases[i].name);
			return 1;
		}
	}
	xsdk_log_write("skin probe: done; xsdk_gpu_close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
