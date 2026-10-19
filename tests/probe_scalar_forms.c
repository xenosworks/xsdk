#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe_gpu.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// What the scalar unit does with the forms milestone 48 step 5's co_issue writes, after mulsc and addsc reading odd
// temps drew wrong on the console (results/2026-10-19-milestone-48/README.md, "Step 5"). Each case's pixel shader loads
// r0 to r63, runs one instruction and exports one value to a 32-bit float target; the probe reads it back, checks it
// against Xenia's model and, where it differs, names what the GPU's value is made of (tools/gen_scalar_forms_probe.py).

typedef enum { OP_MUL, OP_ADD, OP_PICK, OP_FLOOR, OP_FRC, OP_TRUNC, OP_RCP } ProbeOp;

// Where the search for what a value is made of looks first: a named constant and a temp, the lanes of the register
// src3 names, or every input.
typedef enum { DOMAIN_CONSTANT_TEMP, DOMAIN_LANES, DOMAIN_ALL } ProbeDomain;

typedef struct {
	const char *name;
	// What Xenia's model reads the case's value as.
	const char *model;
	uint8_t form;
	uint8_t op;
	uint8_t domain;
	uint32_t instruction[3];
	uint32_t exported[3];
	float expected;
} ProbeCase;

#include "probe_scalar_forms_cases.h"

enum {
	FORMS = sizeof probe_form_names / sizeof probe_form_names[0],
	CASE_COUNT = sizeof probe_cases / sizeof probe_cases[0],
	TEMP_INPUTS = 4 * PROBE_REGISTERS,
	INPUTS = TEMP_INPUTS + 4 * PROBE_NAMED_CONSTANTS,
	// The target's side, a resolve's 32-row tile, and the texel read back.
	SIZE = 32,
	READ_TEXEL = SIZE / 2,
	QUAD_VERTICES = 6,
	// A position, then the colour the vertex shader passes on and the pixel shader ignores.
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	FENCE_TIMEOUT_MS = 1000,
	NAME_BYTES = 8,
	READING_BYTES = 40,
	// Few enough that a case's line stays within the log's.
	READINGS_LOGGED = 3,
	READINGS_TEXT_BYTES = 96,
};

_Static_assert((int)PROBE_CONSTANT_ROWS <= (int)XSDK_PIXEL_CONSTANTS, "the probe's constants below xsdk's");

typedef struct {
	uint32_t a_first;
	uint32_t a_end;
	uint32_t b_first;
	uint32_t b_end;
} Domain;

typedef struct {
	char text[READINGS_TEXT_BYTES];
	uint32_t count;
} Readings;

typedef struct {
	uint32_t passed[FORMS];
	uint32_t failed[FORMS];
} Totals;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
};

// The target's two triangles in clip space.
static const float corners[QUAD_VERTICES][2] = {
	{ -1.0f, 1.0f },
	{ 1.0f, 1.0f },
	{ -1.0f, -1.0f },
	{ -1.0f, -1.0f },
	{ 1.0f, 1.0f },
	{ 1.0f, -1.0f },
};

static const Domain everything = { 0, INPUTS, 0, INPUTS };
static const XsdkColorTarget target = { .format = XSDK_TARGET_32_FLOAT, .tile = 0 };

static float input_values[INPUTS];
static char input_names[INPUTS][NAME_BYTES];
static uint32_t container[PROBE_TEMPLATE_WORDS];
static XsdkShader vertex;
static volatile uint32_t *quad;
static XsdkTexture texture;

// Each temp's lanes, then each named constant's, as the generator lists them.
static void make_inputs(void)
{
	static const char lanes[] = "xyzw";
	for (uint32_t i = 0; i < INPUTS; i++) {
		const bool temp = i < TEMP_INPUTS;
		const uint32_t row = temp ? PROBE_TEMP_ROW + i / 4 : probe_named_constants[(i - TEMP_INPUTS) / 4];
		input_values[i] = probe_constants[row][i % 4];
		snprintf(input_names[i], NAME_BYTES, "%c%u.%c", temp ? 'r' : 'c', (unsigned)(temp ? i / 4 : row), lanes[i % 4]);
	}
}

static uint32_t bits_of(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static bool same_bits(float a, float b)
{
	return bits_of(a) == bits_of(b);
}

static void note(Readings *r, const char *reading)
{
	if (r->count++ >= READINGS_LOGGED)
		return;
	const size_t used = strlen(r->text);
	snprintf(r->text + used, sizeof r->text - used, "%s%s", used ? ", " : "", reading);
}

// Notes the reading format makes of inputs a and b if value is what it gives.
static void try_reading(Readings *r, float value, float result, const char *format, uint32_t a, uint32_t b)
{
	if (!same_bits(value, result))
		return;
	char text[READING_BYTES];
	snprintf(text, sizeof text, format, input_names[a], input_names[b]);
	note(r, text);
}

static float unary(ProbeOp op, float x)
{
	switch (op) {
	case OP_FLOOR:
		return floorf(x);
	case OP_FRC:
		return x - floorf(x);
	case OP_TRUNC:
		return truncf(x);
	default:
		return 1.0f / x;
	}
}

static const char *const unary_formats[][2] = {
	[OP_FLOOR] = { "floor(%s)", "floor(-%s)" },
	[OP_FRC] = { "frc(%s)", "frc(-%s)" },
	[OP_TRUNC] = { "trunc(%s)", "trunc(-%s)" },
	[OP_RCP] = { "rcp(%s)", "rcp(-%s)" },
};

// A sum or product once for both orders where both operands come from one set, as the generator tries them.
static void try_pair(Readings *r, ProbeOp op, bool shared, uint32_t a, uint32_t b, float value)
{
	const float va = input_values[a];
	const float vb = input_values[b];
	const bool once = !shared || b >= a;
	if (op == OP_MUL) {
		if (once) {
			try_reading(r, value, va * vb, "%s * %s", a, b);
			try_reading(r, value, -(va * vb), "-(%s * %s)", a, b);
		}
		return;
	}
	if (once) {
		try_reading(r, value, va + vb, "%s + %s", a, b);
		try_reading(r, value, -va - vb, "-%s - %s", a, b);
	}
	if (!shared || b != a)
		try_reading(r, value, va - vb, "%s - %s", a, b);
	if (!shared)
		try_reading(r, value, -va + vb, "-%s + %s", a, b);
}

static void search(Readings *r, ProbeOp op, const Domain *d, float value)
{
	if (op == OP_PICK)
		return;
	const bool shared = d->a_first == d->b_first && d->a_end == d->b_end;
	for (uint32_t a = d->a_first; a < d->a_end; a++) {
		if (op == OP_MUL || op == OP_ADD) {
			for (uint32_t b = d->b_first; b < d->b_end; b++)
				try_pair(r, op, shared, a, b, value);
			continue;
		}
		try_reading(r, value, unary(op, input_values[a]), unary_formats[op][0], a, a);
		// frc(-x) is frc of another lane's 1 - eighths, so it names nothing.
		if (op != OP_FRC)
			try_reading(r, value, unary(op, -input_values[a]), unary_formats[op][1], a, a);
	}
}

static Domain case_domain(const ProbeCase *c)
{
	const uint32_t reg = c->instruction[2] & 0x3F;
	if (c->domain == DOMAIN_CONSTANT_TEMP)
		return (Domain){ TEMP_INPUTS, INPUTS, 0, TEMP_INPUTS };
	if (c->domain == DOMAIN_LANES)
		return (Domain){ 4 * reg, 4 * reg + 4, 4 * reg, 4 * reg + 4 };
	return everything;
}

// What value is made of: the clear, an input or its negation, then the case's op over its domain, then over every
// input if nothing matched (tools/gen_scalar_forms_probe.py, readings).
static void name_value(const ProbeCase *c, float value, Readings *r)
{
	if (same_bits(value, probe_clear))
		note(r, "the clear, nothing drawn");
	for (uint32_t i = 0; i < INPUTS; i++) {
		try_reading(r, value, input_values[i], "%s", i, i);
		try_reading(r, value, -input_values[i], "-%s", i, i);
	}
	const Domain d = case_domain(c);
	search(r, (ProbeOp)c->op, &d, value);
	if (!r->count && c->domain != DOMAIN_ALL)
		search(r, (ProbeOp)c->op, &everything, value);
}

static bool fence_landed(const char *what)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("scalar forms probe: FAILED, the fence after %s didn't land: 0x%08X", what, (unsigned)status);
	probe_report_cp("scalar forms probe", "after the hang");
	return false;
}

static uint32_t depth_tile(void)
{
	return XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(SIZE, SIZE);
}

// The case's container: the template with its instruction and export in place.
static void build_container(const ProbeCase *c)
{
	memcpy(container, probe_template, sizeof container);
	memcpy(&container[PROBE_INSTRUCTION_WORD], c->instruction, sizeof c->instruction);
	memcpy(&container[PROBE_EXPORT_WORD], c->exported, sizeof c->exported);
}

// Draws the pixel shader over the target and resolves it; false if the fence didn't land.
static bool draw(const XsdkShader *pixel, const char *name)
{
	const float clear[4] = { probe_clear, 0.0f, 0.0f, 0.0f };
	xsdk_gpu_set_targets(SIZE, SIZE, &target, 1, depth_tile());
	xsdk_gpu_set_shaders(&vertex, pixel);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(quad), QUAD_BYTES);
	xsdk_gpu_set_pixel_constants(0, probe_constants[0], PROBE_CONSTANT_ROWS);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	xsdk_gpu_resolve_target(0, &texture, clear);
	return fence_landed(name);
}

static float read_back(void)
{
	float row[SIZE];
	xsdk_texture_download_rows(&texture, 0, READ_TEXEL, 1, row, sizeof row);
	return row[READ_TEXEL];
}

static void log_result(uint32_t number, const ProbeCase *c, float got)
{
	const uint32_t *const w = c->instruction;
	const bool pass = same_bits(got, c->expected);
	char tail[READINGS_TEXT_BYTES + 48] = "";
	if (!pass) {
		Readings r = { .text = "", .count = 0 };
		name_value(c, got, &r);
		if (!r.count)
			snprintf(tail, sizeof tail, "; the GPU's matches no reading");
		else
			snprintf(tail, sizeof tail, "; the GPU's is %s%s", r.text, r.count > READINGS_LOGGED ? ", ..." : "");
		if (r.count > READINGS_LOGGED) {
			const size_t used = strlen(tail);
			snprintf(tail + used, sizeof tail - used, " (%u readings)", (unsigned)r.count);
		}
	}
	xsdk_log_write(
		"scalar forms probe: %u %s: words %08X %08X %08X, scalar op %u, src3_sel %u, swizzle 0x%02X, negate %u, "
		"src3 %u; model %s = %.9g (%08X), GPU %.9g (%08X), %s%s",
		(unsigned)number, c->name, (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)(w[0] >> 26),
		(unsigned)(w[2] >> 29 & 1), (unsigned)(w[1] & 0xFF), (unsigned)(w[1] >> 24 & 1), (unsigned)(w[2] & 0xFF),
		c->model, (double)c->expected, (unsigned)bits_of(c->expected), (double)got, (unsigned)bits_of(got),
		pass ? "PASS" : "FAIL", tail);
}

// False if the fence didn't land, after which the GPU is wedged and the shader stays loaded.
static bool run_case(uint32_t number, const ProbeCase *c, Totals *t)
{
	build_container(c);
	XsdkShader pixel;
	if (!NT_SUCCESS(xsdk_shader_load(&pixel, container, sizeof container))) {
		xsdk_log_write("scalar forms probe: %u %s: its container didn't load, FAIL", (unsigned)number, c->name);
		t->failed[c->form]++;
		return true;
	}
	if (!draw(&pixel, c->name))
		return false;
	xsdk_shader_free(&pixel);
	const float got = read_back();
	log_result(number, c, got);
	t->passed[c->form] += same_bits(got, c->expected);
	t->failed[c->form] += !same_bits(got, c->expected);
	return true;
}

static void log_setup(void)
{
	xsdk_log_write(
		"scalar forms probe: %u cases; each pixel shader loads r0 to r63 from c%u to c%u, runs one instruction and "
		"exports r61.x, the scalar op's, or r60.x, the vector op's, to a %ux%u 32_FLOAT target read back at (%u, %u)",
		(unsigned)CASE_COUNT, (unsigned)PROBE_TEMP_ROW, (unsigned)(PROBE_TEMP_ROW + PROBE_REGISTERS - 1),
		(unsigned)SIZE, (unsigned)SIZE, (unsigned)READ_TEXEL, (unsigned)READ_TEXEL);
	const float *const r1 = probe_constants[PROBE_TEMP_ROW + 1];
	xsdk_log_write(
		"scalar forms probe: r[k] lane l holds 8k + (0, 1, 4, 6)[l] + 1 + (2l + 1) / 8, r1 %.9g %.9g %.9g %.9g; the "
		"rest of c0 to c127 is 0",
		(double)r1[0], (double)r1[1], (double)r1[2], (double)r1[3]);
	for (uint32_t i = 0; i < PROBE_NAMED_CONSTANTS; i++) {
		const float *const v = probe_constants[probe_named_constants[i]];
		xsdk_log_write("scalar forms probe: c%u holds %.9g %.9g %.9g %.9g", (unsigned)probe_named_constants[i],
			(double)v[0], (double)v[1], (double)v[2], (double)v[3]);
	}
	xsdk_log_write(
		"scalar forms probe: a FAIL names what the GPU's value is: the clear, an input or its negation, or the case's "
		"op on inputs, first among the case's own (a constant and a temp, or src3's register's lanes), else among all");
}

static bool make_resources(void)
{
	quad = xsdk_gpu_alloc(QUAD_BYTES);
	if (!quad || !NT_SUCCESS(xsdk_texture_create(&texture, XSDK_FORMAT_R32F, SIZE, SIZE, 1, true)) ||
		!NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }))) {
		xsdk_log_write("scalar forms probe: FAILED, the quad, the texture or the vertex shader couldn't be made");
		return false;
	}
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float values[VERTEX_FLOATS] = { corners[v][0], corners[v][1], 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
		uint32_t words[VERTEX_FLOATS];
		memcpy(words, values, sizeof words);
		for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
			quad[v * VERTEX_FLOATS + i] = words[i];
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(quad), QUAD_BYTES);
	return true;
}

// Clears eDRAM, so the first case starts from the clear as each later one does from its resolve's.
static bool clear_target(void)
{
	const float clear[4] = { probe_clear, 0.0f, 0.0f, 0.0f };
	xsdk_gpu_set_targets(SIZE, SIZE, &target, 1, depth_tile());
	xsdk_gpu_resolve_target(0, &texture, clear);
	return fence_landed("the first clear");
}

int main(void)
{
	make_inputs();
	log_setup();
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("scalar forms probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources() || !clear_target())
		return 1;
	Totals t = { { 0 }, { 0 } };
	for (uint32_t i = 0; i < CASE_COUNT; i++) {
		// The GPU is wedged after a hang: no more GPU work, and the exit's xsdk_gpu_close only times out.
		if (!run_case(i + 1, &probe_cases[i], &t))
			return 1;
	}
	uint32_t passed = 0;
	for (uint32_t f = 0; f < FORMS; f++) {
		xsdk_log_write("scalar forms probe: summary %s: %u PASS, %u FAIL", probe_form_names[f], (unsigned)t.passed[f],
			(unsigned)t.failed[f]);
		passed += t.passed[f];
	}
	status = xsdk_gpu_close();
	xsdk_log_write("scalar forms probe: done, %u cases: %u PASS, %u FAIL; xsdk_gpu_close 0x%08X", (unsigned)CASE_COUNT,
		(unsigned)passed, (unsigned)(CASE_COUNT - passed), (unsigned)status);
	return 0;
}
