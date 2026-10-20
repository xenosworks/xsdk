#ifndef XSDK_XSC_H
#define XSDK_XSC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// xost's compiler: Vulkan or OpenGL SPIR-V to a shader container (xenos-toolchain/docs/xsc.md).

typedef enum {
	XSDK_XSC_OK,
	// xost doesn't lower something the shader does.
	XSDK_XSC_REFUSED,
	// The shader needs more registers than its kind's share of the split.
	XSDK_XSC_REGISTERS,
	// A value doesn't fit the field an instruction or the SPIR-V gives it.
	XSDK_XSC_BAD_VALUE,
	// The SPIR-V isn't one xost understands well enough to say why.
	XSDK_XSC_MALFORMED,
	XSDK_XSC_OUT_OF_MEMORY,
} XsdkXscStatus;

enum {
	XSDK_XSC_MESSAGE_BYTES = 512,
	// The pixel shader's float constant gl_FragCoord.xy adds to the pixel parameters: 0.5, and 0.5 more than a tiled
	// pass's band's top row, as xsdk keeps it (include/xsdk/gpu.h, tiled passes); D3D9's VPOS adds its zw, 0 and the
	// top row. A shader reading gl_FragCoord or VPOS leaves its group of 4 to xsdk.
	XSDK_XSC_FRAG_COORD_CONSTANT = 255,
};

typedef struct {
	// Each kind of shader's share of the split, from r0.
	uint32_t pixel_registers;
	uint32_t vertex_registers;
	// Rewrites the instructions before allocation (xenos-toolchain/xasm/optimize.py).
	bool optimize;
	// Lays a pixel shader's fetches out together, after their coordinates; off, as before milestone 36.
	bool group_fetches;
	// Lowers a vertex shader for instanced draws, as one reading gl_InstanceIndex always is (XSDK_XSC_INSTANCE).
	bool instanced;
} XsdkXscOptions;

// What a shader needs the runtime to set besides its blocks and samplers.
typedef enum {
	// value in constant register reg.
	XSDK_XSC_LITERAL,
	// The uniform array name, regs registers from reg, that a hardware loop indexes.
	XSDK_XSC_ARRAY,
	// The loop count for loop constant reg, from the uniform name.
	XSDK_XSC_LOOP,
	// The loop count for loop constant reg, from the 32-bit int n offset_bytes into the block block, set and binding
	// name: for (i = start; i < n; i += step), so xsdk_xsc_loop_passes(start, step, n) passes, from aL 0 by 1.
	XSDK_XSC_BLOCK_LOOP,
	// User clip plane regs, 0 to 5, from constant register reg: gl_ClipDistance[regs] = dot(gl_Position, the plane)
	// (xenos-toolchain/docs/xsc.md, "Interface conventions").
	XSDK_XSC_CLIP_PLANE,
	// Loop constant reg for loops whose count is data, regs passes, 255, from aL 0 by 1: each leaves on its condition.
	XSDK_XSC_FIXED_LOOP,
} XsdkXscNeedKind;

typedef enum {
	XSDK_XSC_PUSH_CONSTANTS,
	XSDK_XSC_UNIFORM_BUFFER,
	XSDK_XSC_SAMPLER,
	// A sampler2DShadow's compare op, which the runtime writes to the register as the weights of the reference below,
	// equal to and above the depth, 1 or 0 each, then 0.
	XSDK_XSC_COMPARISON,
	// An instanced vertex shader's group, the float constant the runtime writes before each piece of a draw: 1/S, 0.5/S
	// (0 for S a power of two), S, and the piece's first instance, S the vertices or the copies' step an instance
	// (xenos-toolchain/docs/xsc.md, "Interface conventions"); set and binding 0.
	XSDK_XSC_INSTANCE,
} XsdkXscResourceKind;

typedef struct {
	XsdkXscNeedKind kind;
	uint32_t reg;
	uint32_t regs;
	double value[4];
	const char *name;
	// A block loop's count: the block, and where in it.
	XsdkXscResourceKind block;
	uint32_t set;
	uint32_t binding;
	uint32_t offset_bytes;
	// A block loop's i: where it starts, and what each pass adds, 1 or more.
	int32_t start;
	int32_t step;
} XsdkXscNeed;

// Where a block's registers, a sampler's fetch constant, a comparison's register or the instance group landed.
typedef struct {
	XsdkXscResourceKind kind;
	uint32_t set;
	uint32_t binding;
	uint32_t reg;
	uint32_t regs;
} XsdkXscResource;

typedef struct {
	uint8_t *container;
	size_t container_bytes;
	XsdkXscNeed *needs;
	size_t need_count;
	XsdkXscResource *resources;
	size_t resource_count;
	// Why it failed, if it did.
	char message[XSDK_XSC_MESSAGE_BYTES];
} XsdkXscShader;

void xsdk_xsc_default_options(XsdkXscOptions *options);
// Compiles spirv; on success shader holds what xsdk_xsc_free releases.
XsdkXscStatus xsdk_xsc_compile(
	const uint32_t *spirv, size_t spirv_words, const XsdkXscOptions *options, XsdkXscShader *shader);
void xsdk_xsc_free(XsdkXscShader *shader);
// The passes a block loop's loop constant runs where its block holds n, as Shader Model 3's loop counts them:
// ceil((n - start) / step), 0 to 255, the most one counts (xenia:src/xenia/gpu/xenos.h:1086-1097).
uint32_t xsdk_xsc_loop_passes(int32_t start, int32_t step, int32_t n);
// A hash of the compiler's sources, so what it compiled can be told from what a changed compiler would.
const char *xsdk_xsc_version(void);

#ifdef __cplusplus
}
#endif

#endif
