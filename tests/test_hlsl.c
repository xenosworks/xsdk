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
#include "xsdk/texture.h"
#include "xsdk/thread.h"
#include "xsdk/xsc.h"

// D3D9's semantics through glslang's HLSL (docs/scope/50-hlsl.md, step 4): xost's grid_*.hlsl, compiled on the console
// with the optimiser on, each pixel shader drawn behind a vertex shader loaded for it and linked to it by semantic
// (xsdk_shader_link), as five cells where the raster test's frame has nothing, each checked by pixel. One vertex
// shader's TEXCOORD0 and COLOR0 meet a pixel shader's by semantic, with a TEXCOORD1 declared first; VPOS is each
// pixel's whole column and row; VFACE's sign on a clockwise and a counter-clockwise quad, front faces clockwise as on
// D3D9, is reported; DEPTH from TEXCOORD0's x leaves a nearer quad in front of half a cell; another vertex shader's
// TEXCOORD3, COLOR1, TEXCOORD0 and FOG meet a pixel shader reading COLOR1 and TEXCOORD3 alone, in 2 interpolators; a
// loop in SSA form (step 3), a hardware loop an int in $Global counts, each pixel leaving it on its own TEXCOORD0; and
// skinning (step 5), each corner of a quad skinned by four float4x3 bones in vertex constants that its int4
// BLENDINDICES, UBYTE4s, pick, the cell left of the loop's; and binding as D3D9 binds (step 6), samplers at s3 and s7
// bound at slots 3 and 7, a bool at b2 an if reads and an int at i1 a loop counts, set by name through the container's
// table, the cell left of the gaps'.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 10,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_LOGGED = 8,
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
	LOOP_Y = 645,
	// A position, a colour and a coordinate a vertex: POSITION, COLOR0 and TEXCOORD0.
	VERTEX_FLOATS = 10,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_VERTICES = 6,
	// The binding cell, left of the gaps' cell, a quad a half: Low's and High's texels across each, and each its own
	// Bright and Count; slots 3 and 7, as D3D9's SetTexture(3) and (7), and 0 and 1, which samplers packed from 0 read.
	BINDING_X = CELL_X - CELL_WIDTH - 20,
	BINDING_Y = GAPS_Y,
	BINDING_HALF = CELL_WIDTH / 2,
	BINDING_QUAD = 8,
	BINDING_TEXELS = 4,
	LOW_SLOT = 3,
	HIGH_SLOT = 7,
	// Bright's b2 and Count's i1 in a pixel shader's halves (xenos-toolchain/docs/xsc.md, "Interface conventions").
	BRIGHT_BOOLEAN = 128 + 2,
	COUNT_LOOP = 16 + 1,
	DECOY_SLOTS = 2,
	// The link's quad, VPOS's, VFACE's two, DEPTH's, the nearer one over it, the link with gaps', the loop's and the
	// binding's two.
	QUADS = 10,
	VERTEX_WORDS = QUADS * QUAD_VERTICES * VERTEX_FLOATS,
	// VPOS's red and green: the column and row mod 64, times 4.
	VPOS_PERIOD = 64,
	VPOS_STEP = 4,
	// DEPTH's columns this far either side of the middle aren't checked, where its depth meets the nearer quad's.
	DEPTH_MARGIN = 4,
	// The link with gaps' pixel shader's interpolators, COLOR1's and TEXCOORD3's; a map of each semantic to one
	// interpolator in both stages, step 4's first, took 14.
	GAPS_INTERPOLATORS = 2,
	// The loop's count, Count in grid_loop_ps.hlsl's $Global: columns past 12 steps of red run every pass.
	LOOP_COUNT = 12,
	// The skinning cell, left of the loop's; its quad, SKIN_MARGIN in from each side, so one moved covers the clear
	// around it; and its vertices: POSITION, BLENDWEIGHT, BLENDINDICES as a word of UBYTE4s, and TEXCOORD0.
	SKIN_X = CELL_X - CELL_WIDTH - 20,
	SKIN_Y = LOOP_Y,
	SKIN_MARGIN = 8,
	SKIN_QUAD_X = SKIN_X + SKIN_MARGIN,
	SKIN_QUAD_Y = SKIN_Y + SKIN_MARGIN,
	SKIN_QUAD_WIDTH = CELL_WIDTH - 2 * SKIN_MARGIN,
	SKIN_QUAD_HEIGHT = CELL_HEIGHT - 2 * SKIN_MARGIN,
	SKIN_VERTEX_WORDS = 11,
	SKIN_STRIDE_BYTES = SKIN_VERTEX_WORDS * 4,
	SKIN_INDICES_WORD = 8,
	// grid_skin_vs.hlsl's palette, 8 float4x3 bones of 3 registers, a register a row: output j is row j . POSITION.
	BONES = 8,
	BONE_ROWS = 3,
	BONE_REGISTERS = BONES * BONE_ROWS,
	// The bones each corner takes, with weights a half, a quarter and two eighths.
	BONES_A_VERTEX = 4,
};

// Outside the enum: a value past INT_MAX there makes GCC give every constant in it unsigned int, -CELL_HEIGHT too.
static const uint32_t CLEAR_ARGB = 0xFF202020;

// The vertex shaders, then the pixel shaders.
enum {
	SHADER_VERTEX,
	SHADER_GAPS_VERTEX,
	SHADER_SKIN_VERTEX,
	SHADER_LINK,
	SHADER_VPOS,
	SHADER_VFACE,
	SHADER_DEPTH,
	SHADER_GAPS,
	SHADER_LOOP,
	// The skinning's pixel shader, the link's.
	SHADER_SKIN,
	SHADER_BINDING,
	SHADERS,
};

// The binding shader's constants, each found by name in its container's table.
enum {
	BOUND_LOW,
	BOUND_HIGH,
	BOUND_STEP,
	BOUND_BRIGHT,
	BOUND_COUNT,
	BOUND_NAMES,
};

static const struct {
	const char *name;
	uint32_t set;
} bound_names[BOUND_NAMES] = { { "Low", XSDK_SHADER_SAMPLER }, { "High", XSDK_SHADER_SAMPLER },
	{ "Step", XSDK_SHADER_FLOAT }, { "Bright", XSDK_SHADER_BOOL }, { "Count", XSDK_SHADER_INT } };

// The binding cell's textures, 4 texels across: Low's reds, High's greens, and those at slots 0 and 1.
enum {
	LOW_TEXTURE,
	HIGH_TEXTURE,
	DECOY_TEXTURE,
	BINDING_TEXTURES = DECOY_TEXTURE + DECOY_SLOTS,
};

static const uint32_t binding_texels[BINDING_TEXTURES][BINDING_TEXELS] = {
	{ 0xFF300000, 0xFF700000, 0xFFB00000, 0xFFF00000 },
	{ 0xFF00F000, 0xFF00B000, 0xFF007000, 0xFF003000 },
	{ 0xFF08F8F8, 0xFF08F8F8, 0xFF08F8F8, 0xFF08F8F8 },
	{ 0xFFF808F8, 0xFFF808F8, 0xFFF808F8, 0xFFF808F8 },
};

// Step at the float constant the table names: blue's step a pass, and what Bright adds; each half's Count and Bright.
static const float binding_step[4] = { 0.125f, 0.5f, 0.0f, 0.0f };
static const struct {
	uint32_t count;
	uint32_t bright;
} binding_halves[2] = { { 2, 1 }, { 5, 0 } };

static const char *const shader_names[SHADERS] = { "hlsl/grid_vs.hlsl", "hlsl/grid_gaps_vs.hlsl",
	"hlsl/grid_skin_vs.hlsl", "hlsl/grid_link_ps.hlsl", "hlsl/grid_vpos_ps.hlsl", "hlsl/grid_vface_ps.hlsl",
	"hlsl/grid_depth_ps.hlsl", "hlsl/grid_gaps_ps.hlsl", "hlsl/grid_loop_ps.hlsl", "hlsl/grid_link_ps.hlsl",
	"hlsl/grid_binding_ps.hlsl" };

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 32 },
};

static const XsdkVertexElement skin_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 },
	{ .usage = XSDK_USAGE_BLENDWEIGHT, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	{ .usage = XSDK_USAGE_BLENDINDICES, .format = XSDK_VERTEX_UBYTE4, .offset_bytes = SKIN_INDICES_WORD * 4 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 36 },
};

// Each corner's bones, top left, top right, bottom left and bottom right: a pair weighted a half and a quarter whose
// changes from the whole palette's skinning cancel, D and -2D, and a pair weighted an eighth each, E and -E.
static const uint8_t corner_bones[4][BONES_A_VERTEX] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 6, 7 },
	{ 4, 5, 2, 3 } };
static const float skin_weights[BONES_A_VERTEX] = { 0.5f, 0.25f, 0.125f, 0.125f };

// The link's colour, red 64 and green 191, and the nearer quad's red.
static const float link_colour[4] = { 0.25f, 0.75f, 0.0f, 1.0f };
static const float near_colour[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
// The loop's Step at c0: what each pass adds to red and green, and blue's for each pass before the pixel left.
static const float loop_step[4] = { 1.0f / 16.0f, 1.0f / 32.0f, 1.0f / 16.0f, 0.0f };

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
	{ LOOP_Y, false, CELL_X, CELL_WIDTH, 0.5f, link_colour },
	{ BINDING_Y, false, BINDING_X, BINDING_HALF, 0.5f, link_colour },
	{ BINDING_Y, false, BINDING_X + BINDING_HALF, BINDING_HALF, 0.5f, link_colour },
};

// A quad's two triangles, in units of a quad: top left, top right, bottom left, then the other half.
static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };

typedef struct {
	XsdkXscShader compiled[SHADERS];
	// The pixel shaders, and each one's vertex shader, loaded for it and linked to it.
	XsdkShader shaders[SHADERS];
	XsdkShader linked[SHADERS];
	volatile uint32_t *vertices;
	volatile uint32_t *skin_vertices;
	// grid_skin_vs.hlsl's palette at vertex constant 0, and each corner as the CPU skins it.
	float bones[BONE_REGISTERS][4];
	float skinned[4][BONE_ROWS];
	volatile uint32_t *buffers[BUFFERS];
	XsdkRasterState raster;
	XsdkDepthStencilState depth;
	// The loop's loop constant, from its compile's block loop need, and that constant's value for LOOP_COUNT.
	uint32_t loop_constant;
	uint32_t loop_value;
	// The binding shader's constants where its table puts them, -1 for one it doesn't name; its textures, point
	// sampled.
	int32_t bound[BOUND_NAMES];
	XsdkTexture textures[BINDING_TEXTURES];
	XsdkSamplerState point;
	bool has_display;
} Grid;

typedef struct {
	uint32_t link;
	uint32_t vpos;
	uint32_t depth;
	uint32_t gaps;
	uint32_t loop;
	uint32_t skin;
	uint32_t binding;
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

// The palette: each bone the whole skinning, which takes the unit square to the skinning quad in clip space, y up, and
// red from 0.25 at its left to 0.75 at its right, plus its pair's change times 1 and -2 for a pair weighted a half and
// a quarter, 1 and -1 for one weighted an eighth each, so each corner's sum is the whole and a bone read wrong shows.
static void make_bones(float bones[BONE_REGISTERS][4])
{
	const float whole[BONE_ROWS][4] = {
		{ SKIN_QUAD_WIDTH / (WIDTH / 2.0f), 0.0f, 0.0f, (SKIN_QUAD_X - 0.5f) / (WIDTH / 2.0f) - 1.0f },
		{ 0.0f, -SKIN_QUAD_HEIGHT / (HEIGHT / 2.0f), 0.0f, 1.0f - (SKIN_QUAD_Y - 0.5f) / (HEIGHT / 2.0f) },
		{ 0.5f, 0.0f, 0.0f, 0.25f },
	};
	// Each pair's change: x moved, y moved, x scaled, red's ramp steeper.
	static const struct {
		uint32_t row;
		uint32_t column;
		float by;
	} changes[BONES / 2] = { { 0, 3, 0.25f }, { 1, 3, 0.25f }, { 0, 0, 0.125f }, { 2, 0, 0.25f } };
	// A table, not a chain of ?:, which GCC makes an mcrf Xenia skips (tools/xenia_scan.py).
	static const float times[4] = { 1.0f, -2.0f, 1.0f, -1.0f };
	for (uint32_t b = 0; b < BONES; b++) {
		memcpy(bones[b * BONE_ROWS], whole, sizeof whole);
		bones[b * BONE_ROWS + changes[b / 2].row][changes[b / 2].column] += times[b % 4] * changes[b / 2].by;
	}
}

// A corner, 0 to 3 as corner_bones has them, skinned as grid_skin_vs.hlsl skins it: component j the bones' rows j
// dotted with POSITION, weighted.
static void skin_corner(const float bones[BONE_REGISTERS][4], uint32_t corner, float out[BONE_ROWS])
{
	const float pos[4] = { (float)(corner & 1), (float)(corner >> 1), 0.0f, 1.0f };
	for (uint32_t j = 0; j < BONE_ROWS; j++) {
		out[j] = 0.0f;
		for (uint32_t k = 0; k < BONES_A_VERTEX; k++) {
			const float *const row = bones[corner_bones[corner][k] * BONE_ROWS + j];
			out[j] += skin_weights[k] * (row[0] * pos[0] + row[1] * pos[1] + row[2] * pos[2] + row[3] * pos[3]);
		}
	}
}

// Each vertex: its corner of the unit square, BLENDWEIGHT's three weights, its corner's bones as UBYTE4s, and
// TEXCOORD0's x 0 to 1 across.
static void put_skin_vertices(volatile uint32_t *vertices)
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
	return pixel == SHADER_GAPS ? SHADER_GAPS_VERTEX : pixel == SHADER_SKIN ? SHADER_SKIN_VERTEX : SHADER_VERTEX;
}

// The pixel shader, and the vertex shader loaded for it and linked to it.
static bool load_pair(Grid *g, uint32_t pixel)
{
	const uint32_t vertex = vertex_of(pixel);
	const bool skin = pixel == SHADER_SKIN;
	const uint32_t stride = skin ? SKIN_STRIDE_BYTES : STRIDE_BYTES;
	const uint32_t elements = skin ? sizeof skin_layout / sizeof skin_layout[0] : sizeof layout / sizeof layout[0];
	return NT_SUCCESS(load(&g->shaders[pixel], &g->compiled[pixel])) &&
		NT_SUCCESS(load(&g->linked[pixel], &g->compiled[vertex])) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&g->linked[pixel], skin ? skin_layout : layout, elements, &stride)) &&
		NT_SUCCESS(xsdk_shader_link(&g->linked[pixel], &g->shaders[pixel]));
}

// The loop's loop constant and its value for LOOP_COUNT passes, as xsdk works them out from the block's int; false
// without the block loop need, the loop not a hardware loop.
static bool find_loop(Grid *g)
{
	const XsdkXscShader *const c = &g->compiled[SHADER_LOOP];
	for (size_t i = 0; i < c->need_count; i++) {
		const XsdkXscNeed *const n = &c->needs[i];
		if (n->kind != XSDK_XSC_BLOCK_LOOP)
			continue;
		g->loop_constant = n->reg;
		g->loop_value = xsdk_gpu_loop_constant(xsdk_xsc_loop_passes(n->start, n->step, LOOP_COUNT), 0, 1);
		return true;
	}
	return false;
}

// Where the binding shader's table puts each of its constants, by name, as a D3D9 port finds them.
static void find_binding(Grid *g)
{
	for (uint32_t i = 0; i < BOUND_NAMES; i++) {
		XsdkShaderConstant c;
		const bool found =
			xsdk_shader_constant(&g->shaders[SHADER_BINDING], bound_names[i].name, bound_names[i].set, &c);
		g->bound[i] = found ? (int32_t)c.index : -1;
	}
}

static uint32_t bound(const Grid *g, uint32_t i)
{
	return g->bound[i] < 0 ? 0 : (uint32_t)g->bound[i];
}

// The binding cell's textures, linear, and the point sampling they're drawn with.
static bool make_textures(Grid *g)
{
	const XsdkSampler point = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.address_w = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&g->point, &point);
	for (uint32_t i = 0; i < BINDING_TEXTURES; i++) {
		XsdkTexture *const t = &g->textures[i];
		if (!NT_SUCCESS(xsdk_texture_create(t, XSDK_FORMAT_A8R8G8B8, BINDING_TEXELS, 1, 1, false)))
			return false;
		xsdk_texture_upload(t, 0, binding_texels[i], sizeof binding_texels[i]);
		xsdk_gpu_invalidate(t->address, t->size_bytes);
	}
	return true;
}

// The shaders compiled on the console as the raster test compiles its own, the vertices and the frames' buffers.
static bool make_grid(Grid *g)
{
	for (uint32_t i = 0; i < SHADERS; i++) {
		if (!compile_shader(g, i))
			return false;
	}
	if (!find_loop(g))
		return false;
	for (uint32_t i = SHADER_LINK; i < SHADERS; i++) {
		if (!load_pair(g, i))
			return false;
	}
	find_binding(g);
	if (!make_textures(g))
		return false;
	xsdk_gpu_raster_state(&g->raster, &(XsdkRaster){ .cull = XSDK_CULL_NONE, .front_clockwise = true });
	xsdk_gpu_depth_stencil_state(
		&g->depth, &(XsdkDepthStencil){ .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS });
	g->vertices = xsdk_gpu_alloc(VERTEX_WORDS * 4);
	g->skin_vertices = xsdk_gpu_alloc(QUAD_VERTICES * SKIN_STRIDE_BYTES);
	for (uint32_t i = 0; i < BUFFERS; i++)
		g->buffers[i] = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!g->vertices || !g->skin_vertices || !g->buffers[0] || !g->buffers[1])
		return false;
	put_vertices(g->vertices);
	put_skin_vertices(g->skin_vertices);
	make_bones(g->bones);
	for (uint32_t corner = 0; corner < 4; corner++)
		skin_corner(g->bones, corner, g->skinned[corner]);
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

// The skinning's quad, the palette in the vertex constants from c0.
static void draw_skin(const Grid *g)
{
	xsdk_gpu_set_shaders(&g->linked[SHADER_SKIN], &g->shaders[SHADER_SKIN]);
	set_literals(&g->compiled[SHADER_SKIN_VERTEX], false);
	set_literals(&g->compiled[SHADER_SKIN], true);
	xsdk_gpu_set_vertex_constants(0, &g->bones[0][0], BONE_REGISTERS);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(g->skin_vertices), QUAD_VERTICES * SKIN_STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// The binding cell: other textures at 0 and 1, Low's and High's at 3 and 7 as D3D9 binds them, then each half with its
// Bright and Count, set where the table puts them.
static void draw_binding(const Grid *g)
{
	for (uint32_t i = 0; i < DECOY_SLOTS; i++)
		xsdk_gpu_set_texture(i, &g->textures[DECOY_TEXTURE + i], &g->point);
	xsdk_gpu_set_texture(LOW_SLOT, &g->textures[LOW_TEXTURE], &g->point);
	xsdk_gpu_set_texture(HIGH_SLOT, &g->textures[HIGH_TEXTURE], &g->point);
	xsdk_gpu_set_pixel_constants(bound(g, BOUND_STEP), binding_step, 1);
	for (uint32_t half = 0; half < 2; half++) {
		const uint32_t bright = bound(g, BOUND_BRIGHT);
		const uint32_t word = binding_halves[half].bright << bright % 32;
		const uint32_t loop = xsdk_gpu_loop_constant(binding_halves[half].count, 0, 1);
		xsdk_gpu_set_bool_constants(bright / 32, &word, 1);
		xsdk_gpu_set_loop_constants(bound(g, BOUND_COUNT), &loop, 1);
		draw_quads(g, SHADER_BINDING, BINDING_QUAD + half, 1);
	}
}

// One frame of the cells, resolved and waited for; false if the fence didn't land.
static bool draw(const Grid *g, volatile uint32_t *buffer)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_raster(&g->raster);
	draw_quads(g, SHADER_LINK, 0, 1);
	draw_quads(g, SHADER_VPOS, 1, 1);
	draw_quads(g, SHADER_VFACE, 2, 2);
	draw_quads(g, SHADER_GAPS, 6, 1);
	xsdk_gpu_set_pixel_constants(0, loop_step, 1);
	xsdk_gpu_set_loop_constants(g->loop_constant, &g->loop_value, 1);
	draw_quads(g, SHADER_LOOP, 7, 1);
	draw_skin(g);
	draw_binding(g);
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

static uint32_t unorm(float value)
{
	return (uint32_t)(value * 255.0f + 0.5f);
}

// The loop's colour at column x, as grid_loop_ps.hlsl works it out: a step of red and green each pass till red passes
// TEXCOORD0's x, and blue the pass the pixel left at.
static uint32_t loop_colour(uint32_t x)
{
	const float u = ((float)(x - CELL_X) + 0.5f) / CELL_WIDTH;
	float red = 0.0f, green = 0.0f;
	uint32_t i = 0;
	while (i < LOOP_COUNT) {
		red += loop_step[0];
		green += loop_step[1];
		if (red > u)
			break;
		i++;
	}
	return stored(unorm(red), unorm(green), unorm((float)i * loop_step[2]));
}

// The binding cell's colour at column x: Low's red and High's green for the texel there, and blue Count steps and what
// Bright adds.
static uint32_t binding_colour(uint32_t x)
{
	const uint32_t half = (x - BINDING_X) / BINDING_HALF;
	const uint32_t texel = (x - BINDING_X) % BINDING_HALF * BINDING_TEXELS / BINDING_HALF;
	const float blue =
		(float)binding_halves[half].count * binding_step[0] + (float)binding_halves[half].bright * binding_step[1];
	return stored(
		binding_texels[LOW_TEXTURE][texel] >> 16 & 0xFF, binding_texels[HIGH_TEXTURE][texel] >> 8 & 0xFF, unorm(blue));
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

// The skinning's colour at a pixel: inside the quad where the palette puts it, red the skinned z from its left corners
// to its right as the CPU skins them, green COLOR0's and blue TEXCOORD0's x; the clear colour around it.
static uint32_t skin_colour(const Grid *g, uint32_t x, uint32_t y)
{
	if (x < SKIN_QUAD_X || x >= SKIN_QUAD_X + SKIN_QUAD_WIDTH || y < SKIN_QUAD_Y || y >= SKIN_QUAD_Y + SKIN_QUAD_HEIGHT)
		return stored(CLEAR_ARGB >> 16 & 0xFF, CLEAR_ARGB >> 8 & 0xFF, CLEAR_ARGB & 0xFF);
	const float u = ((float)(x - SKIN_QUAD_X) + 0.5f) / SKIN_QUAD_WIDTH;
	const float (*const c)[BONE_ROWS] = g->skinned;
	return stored(unorm(c[0][2] + (c[1][2] - c[0][2]) * u), unorm(0.75f), unorm(u));
}

static void check_frame(const Grid *g, volatile const uint32_t *buffer, uint32_t frame, Wrong *wrong, uint32_t *logged)
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
			y = GAPS_Y + dy;
			wrong->gaps +=
				!pixel_right("gaps", x, y, buffer[tiled_index(x, y, WIDTH)], link, RAMP_TOLERANCE, frame, logged);
			y = LOOP_Y + dy;
			wrong->loop +=
				!pixel_right("loop", x, y, buffer[tiled_index(x, y, WIDTH)], loop_colour(x), TOLERANCE, frame, logged);
			const uint32_t sx = SKIN_X + (x - CELL_X);
			y = SKIN_Y + dy;
			wrong->skin += !pixel_right("skinning", sx, y, buffer[tiled_index(sx, y, WIDTH)], skin_colour(g, sx, y),
				RAMP_TOLERANCE, frame, logged);
			const uint32_t bx = BINDING_X + (x - CELL_X);
			y = BINDING_Y + dy;
			wrong->binding += !pixel_right(
				"binding", bx, y, buffer[tiled_index(bx, y, WIDTH)], binding_colour(bx), TOLERANCE, frame, logged);
			// Last, so the columns DEPTH leaves out are the only ones skipped.
			y = DEPTH_Y + dy;
			const uint32_t middle = CELL_X + CELL_WIDTH / 2;
			if (x + DEPTH_MARGIN > middle && x < middle + DEPTH_MARGIN)
				continue;
			const uint32_t depth = x < middle ? stored(0, 255, 0) : stored(255, 0, ramp(x));
			wrong->depth +=
				!pixel_right("DEPTH", x, y, buffer[tiled_index(x, y, WIDTH)], depth, RAMP_TOLERANCE, frame, logged);
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
	(void)snprintf(name, sizeof name,
		"hlsl: a hardware loop an int in $Global counts, each pixel leaving it once its red passes TEXCOORD0's x, "
		"draws "
		"what it carries and the pass it left at: %u of %u pixels wrong",
		(unsigned)w->loop, (unsigned)pixels);
	check(name, landed && frames && w->loop == 0);
	(void)snprintf(name, sizeof name,
		"hlsl: a quad skinned by float4x3 bones in vertex constants that its corners' int4 BLENDINDICES, UBYTE4s, "
		"pick lands where the palette puts it, the clear around it, and shades as the CPU's skinning of its corners: "
		"%u of %u pixels wrong",
		(unsigned)w->skin, (unsigned)pixels);
	check(name, landed && frames && w->skin == 0);
	(void)snprintf(name, sizeof name,
		"hlsl: samplers at s3 and s7 sample the textures bound at slots 3 and 7, an if reads a bool at b2 and a loop "
		"counts an int at i1, each set by name through the container's table, as D3D9 binds them: %u of %u pixels "
		"wrong",
		(unsigned)w->binding, (unsigned)pixels);
	check(name, landed && frames && w->binding == 0);
}

static void free_grid(Grid *g)
{
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(g->buffers[i]);
	xsdk_gpu_free(g->skin_vertices);
	xsdk_gpu_free(g->vertices);
	for (uint32_t i = 0; i < BINDING_TEXTURES; i++) {
		if (g->textures[i].texels)
			xsdk_texture_free(&g->textures[i]);
	}
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
			check_frame(g, buffer, frame, &wrong, &logged);
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
		char name[320];
		const uint32_t count = g.shaders[SHADER_GAPS].interpolator_count;
		(void)snprintf(name, sizeof name,
			"hlsl: the pixel shader reading COLOR1 and TEXCOORD3 declares %u interpolators in its container, not 14",
			(unsigned)count);
		check(name, count == GAPS_INTERPOLATORS);
		const int32_t *const b = g.bound;
		(void)snprintf(name, sizeof name,
			"hlsl: grid_binding_ps.hlsl's table names Low at fetch constant %d and High at %d, as their s3 and s7, "
			"Step at float constant %d, Bright at boolean constant %d and Count at loop constant %d, "
			"as their b2 and i1",
			(int)b[BOUND_LOW], (int)b[BOUND_HIGH], (int)b[BOUND_STEP], (int)b[BOUND_BRIGHT], (int)b[BOUND_COUNT]);
		check(name,
			b[BOUND_LOW] == LOW_SLOT && b[BOUND_HIGH] == HIGH_SLOT && b[BOUND_STEP] >= 0 &&
				b[BOUND_BRIGHT] == BRIGHT_BOOLEAN && b[BOUND_COUNT] == COUNT_LOOP);
		run(&g);
	}
	xsdk_display_close();
	free_grid(&g);
	check("hlsl: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
