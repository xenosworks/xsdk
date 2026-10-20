// cube: an indexed, textured cube with depth, its matrix set in the vertex shader's constants every frame. The left
// stick turns it; it starts with its red face to the front. It runs until Back, or until the title is left through the
// Guide's Xbox Home.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cube_shaders.h"
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
	CLEAR_ARGB = 0xFF202020,
	FACES = 6,
	VERTICES = FACES * 4,
	INDICES = FACES * 6,
	// Position, colour, then texture coordinates: cube.vert's a_position, a_color and a_uv.
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 12,
	UV_OFFSET_BYTES = 24,
	VERTICES_BYTES = VERTICES * STRIDE_BYTES,
	INDICES_BYTES = INDICES * 2,
	TEXTURE_SIZE = 64,
	CHECK_TEXELS = 8,
	MAX_WIDTH = 1280,
};

// Sticks at rest read a little off centre, so smaller readings count as none.
static const int32_t STICK_DEAD_ZONE = 8000;
static const float TURN_PER_FRAME = 0.05f;
static const float CAMERA_DISTANCE = 3.0f;
static const float FIELD_OF_VIEW = 1.0f;
static const float NEAR = 0.1f;
static const float FAR = 10.0f;

// Each face's outward direction and the two directions across it, +x, -x, +y, -y, +z then -z, and its colour.
static const float faces[FACES][3][3] = {
	{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
	{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
	{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
	{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
	{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
	{ { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
};
static const float face_colours[FACES][3] = {
	{ 0, 1, 0 },
	{ 1, 1, 0 },
	{ 0, 1, 1 },
	{ 1, 0, 1 },
	{ 1, 0, 0 },
	{ 0, 0, 1 },
};

// The vertex shader's inputs, by location: 0 a position, 1 a colour, 2 the first texture coordinates
// (xenos-toolchain/docs/xsc.md, "Interface conventions").
static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = COLOR_OFFSET_BYTES },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = UV_OFFSET_BYTES },
};

// Column-major, a column after another, as GLSL reads a mat4 from constants (xenos-toolchain/docs/xsc.md,
// "Interface conventions").
typedef struct {
	float m[16];
} Matrix;

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	XsdkTexture texture;
	XsdkSamplerState sampler;
	XsdkDepthStencilState depth;
	volatile uint32_t *vertices;
	volatile uint32_t *indices;
} Cube;

static void check_frame(uint32_t address, uint32_t width, uint32_t height);
static void log_checks(void);

// Writes words into GPU memory with volatile 32-bit stores (docs/PITFALLS.md), then drops the GPU's cached copy.
static void write_gpu(volatile uint32_t *memory, const void *data, uint32_t size_bytes)
{
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		uint32_t word;
		memcpy(&word, (const uint8_t *)data + i * 4, 4);
		memory[i] = word;
	}
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
}

// Four corners a face, with the texture across each; two triangles a face. Indices are big-endian, as the CPU is.
static bool make_mesh(Cube *cube)
{
	float vertices[VERTICES][VERTEX_FLOATS];
	uint16_t indices[INDICES];
	for (uint32_t f = 0; f < FACES; f++) {
		for (uint32_t c = 0; c < 4; c++) {
			const float across = c & 1 ? 1.0f : -1.0f;
			const float up = c & 2 ? 1.0f : -1.0f;
			float *const v = vertices[f * 4 + c];
			for (uint32_t axis = 0; axis < 3; axis++) {
				v[axis] = 0.5f * (faces[f][0][axis] + across * faces[f][1][axis] + up * faces[f][2][axis]);
				v[3 + axis] = face_colours[f][axis];
			}
			v[6] = c & 1 ? 1.0f : 0.0f;
			v[7] = c & 2 ? 0.0f : 1.0f;
		}
		const uint16_t base = (uint16_t)(f * 4);
		const uint16_t quad[6] = { base, base + 1, base + 2, base + 2, base + 1, base + 3 };
		memcpy(&indices[f * 6], quad, sizeof quad);
	}
	cube->vertices = xsdk_gpu_alloc(VERTICES_BYTES);
	cube->indices = xsdk_gpu_alloc(INDICES_BYTES);
	if (!cube->vertices || !cube->indices) {
		xsdk_log_write("cube: xsdk_gpu_alloc failed");
		return false;
	}
	write_gpu(cube->vertices, vertices, VERTICES_BYTES);
	write_gpu(cube->indices, indices, INDICES_BYTES);
	return true;
}

// White and grey checks, 0xAARRGGBB as the CPU holds them.
static bool make_texture(Cube *cube)
{
	const NTSTATUS status =
		xsdk_texture_create(&cube->texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIZE, TEXTURE_SIZE, 1, true);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("cube: xsdk_texture_create failed: 0x%08X", (unsigned)status);
		return false;
	}
	static uint32_t texels[TEXTURE_SIZE][TEXTURE_SIZE];
	for (uint32_t y = 0; y < TEXTURE_SIZE; y++)
		for (uint32_t x = 0; x < TEXTURE_SIZE; x++)
			texels[y][x] = (x / CHECK_TEXELS + y / CHECK_TEXELS) % 2 ? 0xFF808080 : 0xFFFFFFFF;
	xsdk_texture_upload(&cube->texture, 0, texels, sizeof texels[0]);
	xsdk_gpu_invalidate(cube->texture.address, cube->texture.size_bytes);
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_WRAP,
		.address_v = XSDK_ADDRESS_WRAP };
	xsdk_texture_sampler_state(&cube->sampler, &sampler);
	return true;
}

static bool make_cube(Cube *cube)
{
	NTSTATUS status = xsdk_shader_load(&cube->vertex, cube_vert.container, cube_vert.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&cube->pixel, cube_frag.container, cube_frag.container_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_set_vertex_layout(
			&cube->vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES });
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("cube: loading the shaders failed: 0x%08X", (unsigned)status);
		return false;
	}
	const XsdkDepthStencil depth = {
#ifdef SAMPLE_BREAK
		// Broken on purpose: no depth test, so the faces drawn last show through those in front.
		.depth_test = false,
#else
		.depth_test = true,
#endif
		.depth_write = true,
		.depth_compare = XSDK_COMPARE_LESS_EQUAL,
	};
	xsdk_gpu_depth_stencil_state(&cube->depth, &depth);
	return make_mesh(cube) && make_texture(cube);
}

static Matrix multiply(const Matrix *a, const Matrix *b)
{
	Matrix product;
	for (uint32_t column = 0; column < 4; column++) {
		for (uint32_t row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (uint32_t k = 0; k < 4; k++)
				sum += a->m[k * 4 + row] * b->m[column * 4 + k];
			product.m[column * 4 + row] = sum;
		}
	}
	return product;
}

// The cube turned by yaw about y and pitch about x, moved away from the eye, then in perspective: depth from 0 at NEAR
// to 1 at FAR, and y up, as Xenos takes clip space.
static Matrix cube_matrix(float yaw, float pitch, float aspect)
{
	const float f = 1.0f / tanf(FIELD_OF_VIEW / 2.0f);
	const float fx = f / aspect;
	const float fz = FAR / (NEAR - FAR);
	const Matrix perspective = { { fx, 0, 0, 0, 0, f, 0, 0, 0, 0, fz, -1, 0, 0, NEAR * fz, 0 } };
	const Matrix away = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -CAMERA_DISTANCE, 1 } };
	const float cp = cosf(pitch);
	const float sp = sinf(pitch);
	const Matrix turn_x = { { 1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1 } };
	const float cy = cosf(yaw);
	const float sy = sinf(yaw);
	const Matrix turn_y = { { cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1 } };
	const Matrix turned = multiply(&turn_x, &turn_y);
	const Matrix placed = multiply(&away, &turned);
	return multiply(&perspective, &placed);
}

static float stick(int16_t value)
{
	return value > STICK_DEAD_ZONE || value < -STICK_DEAD_ZONE ? value / 32768.0f : 0.0f;
}

static void draw(const Cube *cube, const Matrix *matrix, uint32_t address, uint32_t width, uint32_t height)
{
	// A resolve leaves no target, depth test, shaders or texture slot 0 behind, so each frame sets them again.
	xsdk_gpu_set_target(width, height);
	xsdk_gpu_set_depth_stencil(&cube->depth);
	xsdk_gpu_set_shaders(&cube->vertex, &cube->pixel);
	xsdk_built_shader_set_literals(&cube_vert);
	xsdk_built_shader_set_literals(&cube_frag);
	xsdk_gpu_set_vertex_constants(CUBE_VERT_PUSH, matrix->m, CUBE_VERT_PUSH_REGISTERS);
	xsdk_gpu_set_texture(CUBE_FRAG_SAMPLER_0_0, &cube->texture, &cube->sampler);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(cube->vertices), VERTICES_BYTES);
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, xsdk_gpu_address(cube->indices), INDICES, false);
	// Copies the frame into the front buffer, then clears eDRAM's colour to CLEAR_ARGB and its depth to far.
	xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
}

static void run(const Cube *cube, uint32_t width, uint32_t height)
{
	float yaw = 0.0f;
	float pitch = 0.0f;
	for (uint32_t frame = 0;; frame++) {
		INPUT_STATE input = { 0 };
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			break;
		yaw += TURN_PER_FRAME * stick(input.gamepad.thumb_lx);
		pitch -= TURN_PER_FRAME * stick(input.gamepad.thumb_ly);
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("cube: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return;
		}
		const Matrix matrix = cube_matrix(yaw, pitch, (float)width / (float)height);
		draw(cube, &matrix, address, width, height);
		// Frame 0's colour and depth are whatever eDRAM held, so the check is at frame 1.
		if (frame == 1)
			check_frame(address, width, height);
		xsdk_present_end();
	}
}

int main(void)
{
	xsdk_log_write("cube: turn the cube with the left stick; press Back to end");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("cube: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("cube: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	// The title's exit frees GPU memory and gives the GPU back, should anything below fail.
	static Cube cube;
	if (!make_cube(&cube))
		return 1;
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("cube: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	run(&cube, width, height);
	xsdk_present_close();
	xsdk_texture_free(&cube.texture);
	xsdk_gpu_free(cube.indices);
	xsdk_gpu_free(cube.vertices);
	xsdk_shader_free(&cube.pixel);
	xsdk_shader_free(&cube.vertex);
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("cube: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	xsdk_log_write("cube: done");
	log_checks();
	return 0;
}

// The checks: what the title can tell of itself from the log, as it can't see the screen.

enum {
	// The red face times a white or grey check, and nothing of the faces behind it.
	RED_LOW = 0x60,
	OTHER_HIGH = 0x30,
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

// Once the GPU has finished the frame, the middle of the screen shows the red front face, though the blue back face is
// drawn after it, and a corner the clear colour.
static void check_frame(uint32_t address, uint32_t width, uint32_t height)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("cube: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
	const uint32_t middle = frame_pixel(address, width, height, width / 2, height / 2);
	const uint32_t outside = frame_pixel(address, width, height, OUTSIDE_X, OUTSIDE_Y);
	xsdk_log_write(
		"cube: the middle of the first frame held %08X, its corner %08X", (unsigned)middle, (unsigned)outside);
	const bool red =
		(middle >> 16 & 0xFF) > RED_LOW && (middle >> 8 & 0xFF) < OTHER_HIGH && (middle & 0xFF) < OTHER_HIGH;
	check("cube: the first frame shows the red front face in the middle, in front of the blue back face",
		NT_SUCCESS(status) && red);
	check("cube: the first frame's corner holds the clear colour", NT_SUCCESS(status) && outside == CLEAR_ARGB);
}
