#include <algorithm>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "cmake_check_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

namespace
{

enum { SPLASH_MS = 2000 };

// The HLSL pair's target, a resolve's width a multiple of 32, a quad a half, Ramp's texels across each.
constexpr uint32_t TARGET_WIDTH = 64;
constexpr uint32_t TARGET_HEIGHT = 32;
constexpr uint32_t HALVES = 2;
constexpr uint32_t HALF_WIDTH = TARGET_WIDTH / HALVES;
constexpr uint32_t TEXELS = 4;
// D3D9's numbers for quad_ps.hlsl's s2, b1, i3 and c3, a pixel shader's bools from 128 and loops from 16
// (xenos-toolchain/docs/xsc.md, "HLSL").
constexpr int32_t RAMP_SLOT = 2;
constexpr int32_t SWAP_BOOLEAN = 128 + 1;
constexpr int32_t COUNT_LOOP = 16 + 3;
constexpr int32_t STEP_REGISTER = 3;
// Textures at the slots below Ramp's, where samplers packed from 0 would read.
constexpr uint32_t DECOYS = 2;
// POSITION, then TEXCOORD0.
constexpr uint32_t VERTEX_FLOATS = 6;
constexpr uint32_t STRIDE_BYTES = VERTEX_FLOATS * 4;
constexpr uint32_t QUAD_VERTICES = 6;
constexpr uint32_t QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES;
constexpr uint32_t FENCE_TIMEOUT_MS = 1000;
// A colour converts once on the way to the 8-bit target.
constexpr uint32_t TOLERANCE = 1;
constexpr uint32_t MAX_LOGGED = 4;
constexpr uint32_t CLEAR_ARGB = 0xFF202020;
// Ramp's texels, red and green apart so a swap shows, and the decoys'.
constexpr uint32_t ramp_texels[TEXELS] = { 0xFF301000, 0xFF705000, 0xFFB09000, 0xFFF0D000 };
constexpr uint32_t decoy_argb[DECOYS] = { 0xFF08F8F8, 0xFFF808F8 };
// Step: blue's step a pass. Each quad's Swap and Count.
constexpr float step[4] = { 0.125f, 0.0f, 0.0f, 0.0f };
constexpr struct {
	bool swap;
	uint32_t count;
} halves[HALVES] = { { false, 1 }, { true, 3 } };

// quad_vs.hlsl's inputs by semantic: usage, index, format, stream, offset, components and per_instance, all given, as
// C++ warns of a member left out.
constexpr XsdkVertexElement layout[] = {
	{ XSDK_USAGE_POSITION, 0, XSDK_VERTEX_FLOAT4, 0, 0, {}, false },
	{ XSDK_USAGE_TEXCOORD, 0, XSDK_VERTEX_FLOAT2, 0, 16, {}, false },
};

int passed_count;
int failed_count;
bool constructed;

struct Constructed {
	Constructed()
	{
		constructed = true;
	}
};

const Constructed constructed_at_start;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

bool caught()
{
	try {
		throw std::runtime_error("thrown");
	} catch (const std::runtime_error &) {
		return true;
	}
	return false;
}

bool joined()
{
	bool ran = false;
	std::thread thread([&ran] { ran = true; });
	thread.join();
	return ran;
}

// The kernel finds xsdk_add_xex's NAME as a resource named after the title ID, an XDBF database holding it.
bool named()
{
	HANDLE module = 0;
	const uint32_t found = XexGetModuleHandle(nullptr, &module);
	const void *data = nullptr;
	uint32_t size = 0;
	const NTSTATUS status = found == 0 ? XexGetModuleSection(module, TITLE_ID_HEX, &data, &size) : 0;
	xsdk_log_write("cmake: XexGetModuleHandle 0x%08X, XexGetModuleSection(%s) 0x%08X, %u bytes", (unsigned)found,
		TITLE_ID_HEX, (unsigned)status, (unsigned)size);
	if (found != 0 || !NT_SUCCESS(status) || !data)
		return false;
	const std::string_view database(static_cast<const char *>(data), size);
	return database.starts_with("XDBF") && database.find(TITLE_NAME) != std::string_view::npos;
}

// The HLSL pair, its textures, its quads and the frame they're resolved into.
struct Hlsl {
	XsdkShader vertex{};
	XsdkShader pixel{};
	XsdkTexture ramp{};
	XsdkTexture decoys[DECOYS]{};
	XsdkTexture frame{};
	XsdkSamplerState point{};
	volatile uint32_t *vertices = nullptr;
};

// Each quad's two triangles: its corners in clip space, pixel x's centre at x as on D3D9, and TEXCOORD0's x 0 to 1
// across it.
void put_quads(volatile uint32_t *vertices)
{
	constexpr float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
	uint32_t word = 0;
	for (uint32_t half = 0; half < HALVES; half++) {
		for (const auto &corner : corners) {
			const float x = float(half * HALF_WIDTH) + corner[0] * float(HALF_WIDTH) - 0.5f;
			const float y = corner[1] * float(TARGET_HEIGHT) - 0.5f;
			const float vertex[VERTEX_FLOATS] = { x / (TARGET_WIDTH / 2.0f) - 1.0f, 1.0f - y / (TARGET_HEIGHT / 2.0f),
				0.5f, 1.0f, corner[0], 0.5f };
			// GPU memory takes volatile 32-bit stores (docs/PITFALLS.md).
			for (const float f : vertex)
				vertices[word++] = std::bit_cast<uint32_t>(f);
		}
	}
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), HALVES * QUAD_BYTES);
}

bool make_texture(XsdkTexture &texture, const uint32_t (&texels)[TEXELS])
{
	if (!NT_SUCCESS(xsdk_texture_create(&texture, XSDK_FORMAT_A8R8G8B8, TEXELS, 1, 1, false)))
		return false;
	xsdk_texture_upload(&texture, 0, texels, sizeof texels);
	xsdk_gpu_invalidate(texture.address, texture.size_bytes);
	return true;
}

bool make_hlsl(Hlsl &h)
{
	XsdkSampler point{};
	point.mag_filter = point.min_filter = point.mip_filter = XSDK_FILTER_POINT;
	point.address_u = point.address_v = point.address_w = XSDK_ADDRESS_CLAMP;
	xsdk_texture_sampler_state(&h.point, &point);
	if (!NT_SUCCESS(xsdk_shader_load(&h.vertex, quad_vs_hlsl.container, quad_vs_hlsl.container_bytes)) ||
		!NT_SUCCESS(xsdk_shader_load(&h.pixel, quad_ps_hlsl.container, quad_ps_hlsl.container_bytes)) ||
		!NT_SUCCESS(
			xsdk_shader_set_vertex_layout(&h.vertex, layout, sizeof layout / sizeof layout[0], &STRIDE_BYTES)) ||
		!NT_SUCCESS(xsdk_shader_link(&h.vertex, &h.pixel)) || !make_texture(h.ramp, ramp_texels))
		return false;
	for (uint32_t i = 0; i < DECOYS; i++) {
		const uint32_t texels[TEXELS] = { decoy_argb[i], decoy_argb[i], decoy_argb[i], decoy_argb[i] };
		if (!make_texture(h.decoys[i], texels))
			return false;
	}
	h.vertices = xsdk_gpu_alloc(HALVES * QUAD_BYTES);
	if (!h.vertices ||
		!NT_SUCCESS(xsdk_texture_create(&h.frame, XSDK_FORMAT_A8R8G8B8, TARGET_WIDTH, TARGET_HEIGHT, 1, true)))
		return false;
	put_quads(h.vertices);
	return true;
}

// Clears eDRAM, then draws each quad with its Swap, at the boolean constant the table names, and its Count, at i3's
// loop constant as D3D9's SetPixelShaderConstantI(3) sets it; false if the fence didn't land.
bool draw_hlsl(const Hlsl &h, uint32_t swap)
{
	xsdk_gpu_set_target(TARGET_WIDTH, TARGET_HEIGHT);
	xsdk_gpu_resolve_texture(&h.frame, CLEAR_ARGB);
	xsdk_gpu_set_target(TARGET_WIDTH, TARGET_HEIGHT);
	xsdk_gpu_set_shaders(&h.vertex, &h.pixel);
	xsdk_built_shader_set_literals(&quad_vs_hlsl);
	xsdk_built_shader_set_literals(&quad_ps_hlsl);
	for (uint32_t i = 0; i < DECOYS; i++)
		xsdk_gpu_set_texture(i, &h.decoys[i], &h.point);
	xsdk_gpu_set_texture(RAMP_SLOT, &h.ramp, &h.point);
	xsdk_gpu_set_pixel_constants(STEP_REGISTER, step, 1);
	for (uint32_t half = 0; half < HALVES; half++) {
		const uint32_t word = uint32_t(halves[half].swap) << swap % 32;
		const uint32_t loop = xsdk_gpu_loop_constant(halves[half].count, 0, 1);
		xsdk_gpu_set_bool_constants(swap / 32, &word, 1);
		xsdk_gpu_set_loop_constants(COUNT_LOOP, &loop, 1);
		xsdk_gpu_set_vertices(0, xsdk_gpu_address(h.vertices) + half * QUAD_BYTES, QUAD_BYTES);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
	xsdk_gpu_resolve_texture(&h.frame, CLEAR_ARGB);
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

// What column x must hold: Ramp's texel there, red and green swapped where its quad's Swap is set, and blue Count
// steps.
uint32_t expected(uint32_t x)
{
	const auto &half = halves[x / HALF_WIDTH];
	const uint32_t texel = ramp_texels[x % HALF_WIDTH * TEXELS / HALF_WIDTH];
	uint32_t red = texel >> 16 & 0xFF;
	uint32_t green = texel >> 8 & 0xFF;
	if (half.swap)
		std::swap(red, green);
	const auto blue = uint32_t(float(half.count) * step[0] * 255.0f + 0.5f);
	return 0xFF000000 | red << 16 | green << 8 | blue;
}

bool close_to(uint32_t held, uint32_t want)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t a = held >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > TOLERANCE)
			return false;
	}
	return true;
}

// The frame's pixels other than they must be; its resolve left ARGB words (docs/HARDWARE.md, "Render to texture,
// milestone 8").
uint32_t wrong_pixels(const XsdkTexture &frame)
{
	static uint32_t rows[TARGET_HEIGHT][TARGET_WIDTH];
	xsdk_texture_download_rows(&frame, 0, 0, TARGET_HEIGHT, rows, sizeof rows[0]);
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			if (close_to(rows[y][x], expected(x)))
				continue;
			if (wrong++ < MAX_LOGGED)
				xsdk_log_write("cmake: the HLSL pair's pixel (%u, %u) held %08X, not %08X", unsigned(x), unsigned(y),
					unsigned(rows[y][x]), unsigned(expected(x)));
		}
	}
	return wrong;
}

// Where the pixel shader's table puts a constant, -1 where it names none.
int32_t index_of(const XsdkShader &shader, const char *name, uint32_t set)
{
	XsdkShaderConstant c;
	return xsdk_shader_constant(&shader, name, set, &c) ? int32_t(c.index) : -1;
}

void free_hlsl(Hlsl &h)
{
	xsdk_texture_free(&h.frame);
	xsdk_gpu_free(h.vertices);
	for (auto &decoy : h.decoys)
		xsdk_texture_free(&decoy);
	xsdk_texture_free(&h.ramp);
	if (h.pixel.microcode)
		xsdk_shader_free(&h.pixel);
	if (h.vertex.microcode)
		xsdk_shader_free(&h.vertex);
}

// An HLSL pair built by xsdk_add_shaders, bound as D3D9 binds it (docs/scope/50-hlsl.md, "Done when"), each pixel
// checked, and its table checked for D3D9's numbers.
void check_hlsl()
{
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status))
		xsdk_log_write("cmake: xsdk_gpu_open failed: 0x%08X", unsigned(status));
	Hlsl h;
	const bool made = NT_SUCCESS(status) && make_hlsl(h);
	const int32_t ramp = index_of(h.pixel, "Ramp", XSDK_SHADER_SAMPLER);
	const int32_t swap = index_of(h.pixel, "Swap", XSDK_SHADER_BOOL);
	const int32_t count = index_of(h.pixel, "Count", XSDK_SHADER_INT);
	const int32_t at = index_of(h.pixel, "Step", XSDK_SHADER_FLOAT);
	const bool landed = made && draw_hlsl(h, uint32_t(std::max(swap, 0)));
	const uint32_t wrong = landed ? wrong_pixels(h.frame) : TARGET_WIDTH * TARGET_HEIGHT;
	char name[400];
	(void)snprintf(name, sizeof name,
		"cmake: an HLSL pair built by xsdk_add_shaders draws as D3D9 binds it, Ramp at s2 bound at slot 2, Swap at b1 "
		"set by name, Count at i3 at loop constant 19 and Step at c3, its table naming Ramp at fetch constant %d, Swap "
		"at boolean constant %d, Count at loop constant %d and Step at float constant %d: %u of %u pixels wrong",
		int(ramp), int(swap), int(count), int(at), unsigned(wrong), unsigned(TARGET_WIDTH * TARGET_HEIGHT));
	check(name,
		landed && wrong == 0 && ramp == RAMP_SLOT && swap == SWAP_BOOLEAN && count == COUNT_LOOP &&
			at == STEP_REGISTER);
	free_hlsl(h);
	if (!NT_SUCCESS(status))
		return;
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("cmake: xsdk_gpu_close failed: 0x%08X", unsigned(status));
}

} // namespace

int main()
{
	xsdk_log_write("cmake check title");
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(xsdk_splash_show(SPLASH_MS)));
	check("cmake: a static constructor ran before main", constructed);
	check("cmake: an exception was thrown and caught", caught());
	check("cmake: a std::thread ran and joined", joined());
	check("cmake: the .xex holds the title name xsdk_add_xex gave it", named());
	check_hlsl();
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
