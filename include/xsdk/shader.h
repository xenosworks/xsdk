#ifndef XSDK_SHADER_H
#define XSDK_SHADER_H

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shaders from xost's containers, libxenon's format (xenos-toolchain/docs/container.md), placed in GPU memory.

enum {
	XSDK_SHADER_VERTEX = 0,
	XSDK_SHADER_PIXEL = 1,
	// D3D9's declaration usages, numbered as libxenon has them (libxenon:drivers/xenos/xe.h:159-172).
	XSDK_USAGE_POSITION = 0,
	XSDK_USAGE_BLENDWEIGHT = 1,
	XSDK_USAGE_BLENDINDICES = 2,
	XSDK_USAGE_NORMAL = 3,
	XSDK_USAGE_PSIZE = 4,
	XSDK_USAGE_TEXCOORD = 5,
	XSDK_USAGE_TANGENT = 6,
	XSDK_USAGE_BINORMAL = 7,
	XSDK_USAGE_TESSFACTOR = 8,
	XSDK_USAGE_POSITIONT = 9,
	XSDK_USAGE_COLOR = 10,
	XSDK_USAGE_FOG = 11,
	XSDK_USAGE_DEPTH = 12,
	XSDK_USAGE_SAMPLE = 13,
	// Each takes a vertex fetch constant, 95 down.
	XSDK_VERTEX_STREAMS = 16,
};

// Vertex formats, as Vulkan's; vertices are 32-bit words, each component after the first in the bits above it.
enum {
	XSDK_VERTEX_FLOAT1,
	XSDK_VERTEX_FLOAT2,
	XSDK_VERTEX_FLOAT3,
	XSDK_VERTEX_FLOAT4,
	// Half floats, two to a word.
	XSDK_VERTEX_HALF2,
	XSDK_VERTEX_HALF4,
	// Four bytes to a word: unsigned or signed, normalised to 0 to 1 or -1 to 1, or whole numbers.
	XSDK_VERTEX_UBYTE4N,
	XSDK_VERTEX_BYTE4N,
	XSDK_VERTEX_UBYTE4,
	XSDK_VERTEX_BYTE4,
	// Two 16-bit numbers to a word, likewise.
	XSDK_VERTEX_USHORT2N,
	XSDK_VERTEX_SHORT2N,
	XSDK_VERTEX_USHORT2,
	XSDK_VERTEX_SHORT2,
	XSDK_VERTEX_USHORT4N,
	XSDK_VERTEX_SHORT4N,
	XSDK_VERTEX_USHORT4,
	XSDK_VERTEX_SHORT4,
	XSDK_VERTEX_FORMATS,
};

typedef struct {
	volatile uint32_t *microcode;
	uint32_t address;
	uint32_t dword_count;
	uint32_t type;
	// SQ_PROGRAM_CNTL and SQ_CONTEXT_MISC bits the shader needs.
	uint32_t program_control;
	uint32_t context_misc;
	// The container's vertex fetch patch records, interpolator records, export slots and microcode; the container must
	// outlive the shader.
	const uint32_t *fetches;
	uint32_t fetch_count;
	const uint32_t *interpolators;
	uint32_t interpolator_count;
	const uint32_t *export_slots;
	uint32_t export_slot_count;
	const uint32_t *source;
	// The container's constant table from its header, and the bytes from there to the container's end; NULL for none.
	const uint32_t *table;
	uint32_t table_bytes;
	// An instanced vertex shader's group, the float constant its instanced draws write (include/xsdk/xsc.h,
	// XSDK_XSC_INSTANCE), from the _instance its container's constant table names.
	bool instanced;
	uint32_t instance_constant;
} XsdkShader;

// The register sets a constant table holds constants in (XenosRecomp:XenosRecomp/constant_table.h).
enum {
	XSDK_SHADER_BOOL,
	XSDK_SHADER_INT,
	XSDK_SHADER_FLOAT,
	XSDK_SHADER_SAMPLER,
};

// Where a constant lands: its first float constant and how many it takes, its boolean constant, bit index % 32 of word
// index / 32 for xsdk_gpu_set_bool_constants, its loop constant, or its texture's fetch constant before
// xsdk_shader_move_textures; and its rows, columns and array elements as the shader declares it.
typedef struct {
	uint32_t index;
	uint32_t count;
	uint32_t rows;
	uint32_t columns;
	uint32_t elements;
} XsdkShaderConstant;

// Which of a format's components an attribute's x, y, z or w reads, counted as the formats above lay them out; 0 reads
// its own, so x reads the first.
enum {
	XSDK_COMPONENT_OWN,
	XSDK_COMPONENT_X,
	XSDK_COMPONENT_Y,
	XSDK_COMPONENT_Z,
	XSDK_COMPONENT_W,
};

// One attribute of a vertex: the shader input it feeds, its format, its stream, where it sits in the stream, and the
// XSDK_COMPONENT_* each of x to w reads, so data whose components lie in another order is fetched as it is. An
// instanced vertex shader's attribute per_instance is fetched at its instance in the stream, each instance an element
// of the stream's stride, in its instanced draws (include/xsdk/gpu.h).
typedef struct {
	uint32_t usage;
	uint32_t usage_index;
	uint32_t format;
	uint32_t stream;
	uint32_t offset_bytes;
	uint8_t components[4];
	bool per_instance;
} XsdkVertexElement;

// Copies the container's microcode into GPU memory.
NTSTATUS xsdk_shader_load(XsdkShader *shader, const uint32_t *container, uint32_t size_bytes);
// Patches a vertex shader's fetches for this layout and each stream's stride, as libxenon does; not while it may run.
// An element per_instance is refused for a shader that isn't instanced.
NTSTATUS xsdk_shader_set_vertex_layout(
	XsdkShader *shader, const XsdkVertexElement *elements, uint32_t element_count, const uint32_t *stream_strides);
// Moves the shader's texture fetches up by first_slot fetch constants, from the container's, so both stages' textures
// fit among the 32 slots; not while it may run.
NTSTATUS xsdk_shader_move_textures(XsdkShader *shader, uint32_t first_slot);
// Links an HLSL vertex shader's outputs to a pixel shader's inputs by semantic, as D3D9 does: each goes to the pixel
// shader's interpolator for its semantic, those it doesn't read nowhere, and an input no output feeds reads what the
// GPU holds. Not while the vertex shader may run; one linked already to the same interpolators is left as it is, so
// a vertex shader drawn with pixel shaders that read different semantics is loaded for each. GLSL's link by location
// and need none; one of each is refused.
NTSTATUS xsdk_shader_link(XsdkShader *vertex, const XsdkShader *pixel);
// The constant named name in register set set, XSDK_SHADER_*, as its container's table gives it, so a port binds HLSL's
// constants by name; false where the shader has none of that name there. An HLSL bool an if reads is a boolean
// constant, and an int a loop counts a loop constant, written xsdk_gpu_loop_constant(n, 0, 1) for n; read as a value,
// either is a float constant, a bool 1.0 or 0.0 (xenos-toolchain/docs/xsc.md, "Interface conventions").
bool xsdk_shader_constant(const XsdkShader *shader, const char *name, uint32_t set, XsdkShaderConstant *constant);
void xsdk_shader_free(XsdkShader *shader);

#ifdef __cplusplus
}
#endif

#endif
