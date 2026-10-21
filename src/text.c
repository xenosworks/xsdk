#include "xsdk/text.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_private.h"
#include "text_font.h"
#include "text_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// src/text_font.h: tools/gen_font.py --output src/text_font.h assets/fonts/10x20.bdf
// src/text_shaders.h: tools/build_shaders.py --output src/text_shaders.h src/text.vert src/text.frag

enum {
	MAX_SCALE = 8,
	// The atlas: a glyph a cell, 102 across and 12 down, each texel 0 or 255.
	ATLAS_WIDTH = 1024,
	ATLAS_HEIGHT = 256,
	ATLAS_COLUMNS = ATLAS_WIDTH / XSDK_TEXT_CELL_WIDTH,
	LIT = 0xFF,
	// A row's leftmost pixel (src/text_font.h).
	LEFT_BIT = 0x8000,
	// A glyph's quad: four corners, each a position and an atlas coordinate, two floats each.
	CORNER_FLOATS = 4,
	GLYPH_FLOATS = 4 * CORNER_FLOATS,
	STRIDE_BYTES = CORNER_FLOATS * 4,
	GLYPH_BYTES = GLYPH_FLOATS * 4,
	// The ring: 4,096 glyphs in 64 KB blocks.
	RING_BYTES = 0x40000,
	RING_BLOCKS = 4,
	BLOCK_GLYPHS = RING_BYTES / RING_BLOCKS / GLYPH_BYTES,
	INDICES_PER_GLYPH = 6,
	WAIT_TIMEOUT_MS = 1000,
	// What a malformed byte reads as, the replacement character, which the font's set lacks.
	REPLACEMENT = 0xFFFD,
};

_Static_assert(TEXT_FONT_GLYPHS <= ATLAS_COLUMNS * (ATLAS_HEIGHT / XSDK_TEXT_CELL_HEIGHT), "the atlas's cells");

// A glyph's two triangles, corners top left, top right, bottom left and bottom right.
static const uint32_t quad_indices[INDICES_PER_GLYPH] = { 0, 1, 2, 2, 1, 3 };

// A glyph's corner: its position at location 0 and its atlas coordinate at 2, TEXCOORD 0 (src/text.vert).
static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 8 },
};

typedef struct {
	XsdkTexture atlas;
	XsdkSamplerState sampler;
	XsdkShader vertex;
	XsdkShader pixel;
	// The ring, then the index list, written with volatile stores.
	volatile uint32_t *ring;
	uint32_t ring_address;
	XsdkRepeatedIndices indices;
	XsdkBlendState blend;
	XsdkDepthStencilState depth_stencil;
	XsdkRasterState raster;
	// The block being written, the glyphs in it so far, and the fence queued as each block was last left.
	uint32_t block;
	uint32_t block_glyphs;
	uint32_t fences[RING_BLOCKS];
} Text;

static Text text;

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// Glyph i's cell: column i % ATLAS_COLUMNS, row i / ATLAS_COLUMNS.
static void put_cell(uint8_t *texels, uint32_t glyph)
{
	uint8_t *const cell = &texels[glyph / ATLAS_COLUMNS * XSDK_TEXT_CELL_HEIGHT * ATLAS_WIDTH +
		glyph % ATLAS_COLUMNS * XSDK_TEXT_CELL_WIDTH];
	for (uint32_t row = 0; row < XSDK_TEXT_CELL_HEIGHT; row++) {
		const uint32_t bits = text_font_rows[glyph][row];
		for (uint32_t column = 0; column < XSDK_TEXT_CELL_WIDTH; column++)
			cell[row * ATLAS_WIDTH + column] = bits << column & LEFT_BIT ? LIT : 0;
	}
}

static NTSTATUS make_atlas(void)
{
	NTSTATUS status = xsdk_texture_create(&text.atlas, XSDK_FORMAT_R8, ATLAS_WIDTH, ATLAS_HEIGHT, 1, true);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("text: xsdk_texture_create failed: 0x%08X", (unsigned)status);
		return status;
	}
	uint8_t *const texels = calloc(ATLAS_WIDTH * ATLAS_HEIGHT, 1);
	if (!texels) {
		xsdk_log_write("text: calloc(%u) failed", (unsigned)(ATLAS_WIDTH * ATLAS_HEIGHT));
		return STATUS_NO_MEMORY;
	}
	for (uint32_t glyph = 0; glyph < TEXT_FONT_GLYPHS; glyph++)
		put_cell(texels, glyph);
	xsdk_texture_upload(&text.atlas, 0, texels, ATLAS_WIDTH);
	free(texels);
	xsdk_gpu_invalidate(text.atlas.address, text.atlas.size_bytes);
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.address_w = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&text.sampler, &sampler);
	return STATUS_SUCCESS;
}

static NTSTATUS load_shaders(void)
{
	NTSTATUS status = xsdk_shader_load(&text.vertex, text_vert.container, text_vert.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&text.pixel, text_frag.container, text_frag.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_set_vertex_layout(
			&text.vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES });
	if (!NT_SUCCESS(status))
		xsdk_log_write("text: loading the shaders failed: 0x%08X", (unsigned)status);
	return status;
}

// The ring, and after it each block's index list: a glyph's six indices, each glyph's four corners on.
static NTSTATUS make_ring(void)
{
	const uint32_t index_bytes = xsdk_gpu_repeated_index_bytes(quad_indices, INDICES_PER_GLYPH, BLOCK_GLYPHS);
	text.ring = xsdk_gpu_alloc(RING_BYTES + index_bytes);
	if (!text.ring) {
		xsdk_log_write("text: xsdk_gpu_alloc(%u) failed", (unsigned)(RING_BYTES + index_bytes));
		return STATUS_NO_MEMORY;
	}
	text.ring_address = xsdk_gpu_address(text.ring);
	xsdk_gpu_repeat_indices(&text.indices, text.ring + RING_BYTES / 4, quad_indices, INDICES_PER_GLYPH, BLOCK_GLYPHS);
	assert(text.indices.copies == BLOCK_GLYPHS && !text.indices.indices_32_bit);
	xsdk_gpu_invalidate(text.indices.address, index_bytes);
	return STATUS_SUCCESS;
}

// Everything open made, in reverse.
static void release(void)
{
	xsdk_gpu_free(text.ring);
	xsdk_shader_free(&text.pixel);
	xsdk_shader_free(&text.vertex);
	xsdk_texture_free(&text.atlas);
	text = (Text){ .ring = NULL };
}

NTSTATUS xsdk_text_open(void)
{
	assert(!text.ring);
	NTSTATUS status = make_atlas();
	if (NT_SUCCESS(status))
		status = load_shaders();
	if (NT_SUCCESS(status))
		status = make_ring();
	if (!NT_SUCCESS(status)) {
		release();
		return status;
	}
	// Over what's drawn by the colour's alpha.
	const XsdkBlend blend = { .color_source = XSDK_BLEND_SRC_ALPHA,
		.color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
		.color_op = XSDK_BLEND_OP_ADD,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
		.alpha_op = XSDK_BLEND_OP_ADD,
		.write_mask = XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&text.blend, &blend);
	xsdk_gpu_depth_stencil_state(&text.depth_stencil, &(const XsdkDepthStencil){ .depth_test = false });
	xsdk_gpu_raster_state(&text.raster, &(const XsdkRaster){ .cull = XSDK_CULL_NONE });
	return STATUS_SUCCESS;
}

void xsdk_text_close(void)
{
	if (!text.ring)
		return;
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), WAIT_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		// The GPU may still read the ring and the atlas.
		xsdk_log_write("text: the GPU didn't finish: 0x%08X; keeping its memory", (unsigned)status);
		text = (Text){ .ring = NULL };
		return;
	}
	release();
}

// The code point at *s, moving s past it; a malformed byte is one REPLACEMENT.
static uint32_t next_code_point(const uint8_t **s)
{
	const uint8_t *const p = *s;
	*s = p + 1;
	if (p[0] < 0x80)
		return p[0];
	const uint32_t length = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC0 ? 2 : 0;
	// The least each length may hold, so an overlong form is malformed.
	static const uint32_t least[] = { 0, 0, 0x80, 0x800, 0x10000 };
	if (!length || p[0] >= 0xF8)
		return REPLACEMENT;
	uint32_t point = p[0] & (0x7F >> length);
	for (uint32_t i = 1; i < length; i++) {
		// A NUL ends it here too.
		if ((p[i] & 0xC0) != 0x80)
			return REPLACEMENT;
		point = point << 6 | (p[i] & 0x3F);
	}
	if (point < least[length] || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF))
		return REPLACEMENT;
	*s = p + length;
	return point;
}

// C0 and C1 controls and DEL, which draw nothing; '\n' is taken first.
static bool is_control(uint32_t point)
{
	return point < 0x20 || (point >= 0x7F && point < 0xA0);
}

// The glyph for a code point, or the default, glyph 0.
static uint32_t glyph_of(uint32_t point)
{
	uint32_t low = 0;
	uint32_t high = TEXT_FONT_GLYPHS;
	while (low < high) {
		const uint32_t middle = (low + high) / 2;
		if (text_font_code_points[middle] < point)
			low = middle + 1;
		else
			high = middle;
	}
	return low < TEXT_FONT_GLYPHS && text_font_code_points[low] == point ? low : 0;
}

static float channel(uint32_t value)
{
	return (float)(value & 0xFF) / 255.0f;
}

static void set_state(uint32_t argb)
{
	float view_width;
	float view_height;
	xsdk_gpu_viewport_size(&view_width, &view_height);
	// Pixels from the viewport's top left to clip space, y up.
	const float to_clip[4] = { 2.0f / view_width, -2.0f / view_height, -1.0f, 1.0f };
	const float colour[4] = { channel(argb >> 16), channel(argb >> 8), channel(argb), channel(argb >> 24) };
	xsdk_gpu_set_shaders(&text.vertex, &text.pixel);
	xsdk_built_shader_set_literals(&text_vert);
	xsdk_built_shader_set_literals(&text_frag);
	xsdk_gpu_set_vertex_constants(TEXT_VERT_PUSH, to_clip, TEXT_VERT_PUSH_REGISTERS);
	xsdk_gpu_set_pixel_constants(TEXT_FRAG_PUSH, colour, TEXT_FRAG_PUSH_REGISTERS);
	xsdk_gpu_set_texture(TEXT_FRAG_SAMPLER_0_0, &text.atlas, &text.sampler);
	xsdk_gpu_set_blend(&text.blend);
	xsdk_gpu_set_depth_stencil(&text.depth_stencil);
	xsdk_gpu_set_raster(&text.raster);
}

// Writes the glyph's quad into the block, its atlas coordinates on its cell's edges.
static void put_glyph(uint32_t glyph, float left, float top, float width, float height)
{
	const float u = (float)(glyph % ATLAS_COLUMNS * XSDK_TEXT_CELL_WIDTH) / ATLAS_WIDTH;
	const float v = (float)(glyph / ATLAS_COLUMNS * XSDK_TEXT_CELL_HEIGHT) / ATLAS_HEIGHT;
	const float u_right = u + (float)XSDK_TEXT_CELL_WIDTH / ATLAS_WIDTH;
	const float v_bottom = v + (float)XSDK_TEXT_CELL_HEIGHT / ATLAS_HEIGHT;
	const float right = left + width;
	const float bottom = top + height;
	const float corners[GLYPH_FLOATS] = { left, top, u, v, right, top, u_right, v, left, bottom, u, v_bottom, right,
		bottom, u_right, v_bottom };
	volatile uint32_t *const slot = &text.ring[(text.block * BLOCK_GLYPHS + text.block_glyphs) * GLYPH_FLOATS];
	for (uint32_t i = 0; i < GLYPH_FLOATS; i++)
		slot[i] = float_bits(corners[i]);
	text.block_glyphs++;
}

// Draws the block's glyphs from first on, written since the last draw.
static void draw_piece(uint32_t first)
{
	const uint32_t count = text.block_glyphs - first;
	if (!count)
		return;
	const uint32_t address = text.ring_address + (text.block * BLOCK_GLYPHS + first) * GLYPH_BYTES;
	xsdk_gpu_invalidate(address, count * GLYPH_BYTES);
	xsdk_gpu_set_vertices(0, address, count * GLYPH_BYTES);
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, text.indices.address, count * INDICES_PER_GLYPH, false);
}

// Leaving a block queues a fence for it, and the next is written only once the GPU has passed its own, as the
// resolve's rectangles are reused (src/gpu.c, rectangle_slot).
static void next_block(void)
{
	text.fences[text.block] = xsdk_gpu_fence();
	text.block = (text.block + 1) % RING_BLOCKS;
	text.block_glyphs = 0;
	const NTSTATUS status = xsdk_gpu_wait(text.fences[text.block], WAIT_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("text: xsdk_gpu_wait for the ring's next block failed: 0x%08X", (unsigned)status);
}

void xsdk_text_draw(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8)
{
	assert(text.ring && scale >= 1 && scale <= MAX_SCALE);
	// A recorded draw could outlive its block.
	assert(!xsdk_gpu_recording() && !xsdk_gpu_tiling());
	set_state(argb);
	const float width = (float)(XSDK_TEXT_CELL_WIDTH * scale);
	const float height = (float)(XSDK_TEXT_CELL_HEIGHT * scale);
	// Pixel centres sit on whole coordinates, D3D9's (src/gpu_setup.c; xenia:src/xenia/gpu/xenos.h:861-868), so a
	// cell's edges are half a pixel up and left of its first pixel's.
	const float left = (float)x - 0.5f;
	float top = (float)y - 0.5f;
	float pen = left;
	uint32_t first = text.block_glyphs;
	for (const uint8_t *s = (const uint8_t *)utf8; *s;) {
		const uint32_t point = next_code_point(&s);
		if (point == '\n') {
			pen = left;
			top += height;
			continue;
		}
		if (is_control(point))
			continue;
		if (text.block_glyphs == BLOCK_GLYPHS) {
			draw_piece(first);
			next_block();
			first = 0;
		}
		put_glyph(glyph_of(point), pen, top, width, height);
		pen += width;
	}
	draw_piece(first);
}

void xsdk_text_measure(const char *utf8, uint32_t scale, uint32_t *width, uint32_t *height)
{
	uint32_t longest = 0;
	uint32_t cells = 0;
	uint32_t lines = 1;
	for (const uint8_t *s = (const uint8_t *)utf8; *s;) {
		const uint32_t point = next_code_point(&s);
		if (point == '\n') {
			lines++;
			cells = 0;
		} else if (!is_control(point) && ++cells > longest) {
			longest = cells;
		}
	}
	*width = longest * XSDK_TEXT_CELL_WIDTH * scale;
	*height = lines * XSDK_TEXT_CELL_HEIGHT * scale;
}
