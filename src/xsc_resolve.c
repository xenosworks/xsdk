#include <stdio.h>
#include <string.h>

#include "xsc_private.h"

// Types and the shader interface, as xenos-toolchain/xsc/resolve.py resolves them.

enum {
	SC_UNIFORM_CONSTANT = 0,
	SC_INPUT = 1,
	SC_UNIFORM = 2,
	SC_OUTPUT = 3,
	SC_PUSH_CONSTANT = 9,
	BUILTIN_POSITION = 0,
	BUILTIN_POINT_SIZE = 1,
	BUILTIN_VERTEX_ID = 5,
	BUILTIN_INSTANCE_ID = 6,
	BUILTIN_FRAG_COORD = 15,
	BUILTIN_POINT_COORD = 16,
	BUILTIN_FRONT_FACING = 17,
	BUILTIN_FRAG_DEPTH = 22,
	// The console loads float constants from memory in whole groups of 4 (docs/HARDWARE.md, "Constants loaded from
	// memory go in groups of 4 too").
	CONSTANT_GROUP = 4,
	DEFAULT_STRIDE = 16,
	NAME_BYTES = 32,
	// A texture fetch names one of 32 fetch constants (xenia:src/xenia/gpu/ucode.h:849).
	TEXTURE_SLOTS = 32,
};

static const char *const type_names[] = { "none", "void", "bool", "int", "float", "vector", "matrix", "image",
	"sampler", "sampledimage", "array", "struct", "pointer", "function" };

const char *xsdk_xsc_type_kind_name(XscTypeKind kind)
{
	return type_names[kind];
}

XscType *xsdk_xsc_resolve_types(Xsc *x, const XscModule *m)
{
	XscType *const types = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *types);
	if (!types)
		return NULL;
	for (uint32_t id = 0; id < m->id_limit; id++)
		types[id].dim = XSC_NONE;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		uint32_t count;
		const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
		XscType t = { .dim = XSC_NONE };
		switch (ins->opcode) {
		case SPV_TYPE_VOID:
			t.kind = XSC_TYPE_VOID;
			break;
		case SPV_TYPE_BOOL:
			t.kind = XSC_TYPE_BOOL;
			break;
		case SPV_TYPE_INT:
			t.kind = XSC_TYPE_INT;
			break;
		case SPV_TYPE_FLOAT:
			t.kind = XSC_TYPE_FLOAT;
			break;
		case SPV_TYPE_VECTOR:
		case SPV_TYPE_MATRIX:
		case SPV_TYPE_ARRAY:
			t.kind = ins->opcode == SPV_TYPE_VECTOR ? XSC_TYPE_VECTOR
				: ins->opcode == SPV_TYPE_MATRIX    ? XSC_TYPE_MATRIX
													: XSC_TYPE_ARRAY;
			t.comp = xsdk_xsc_operand(ins, 0);
			t.count = xsdk_xsc_operand(ins, 1);
			break;
		case SPV_TYPE_IMAGE:
			t.kind = XSC_TYPE_IMAGE;
			t.comp = xsdk_xsc_operand(ins, 0);
			t.dim = xsdk_xsc_operand(ins, 1);
			t.depth = xsdk_xsc_operand(ins, 2) == 1;
			break;
		case SPV_TYPE_SAMPLER:
			t.kind = XSC_TYPE_SAMPLER;
			break;
		case SPV_TYPE_SAMPLED_IMAGE:
			t.kind = XSC_TYPE_SAMPLED_IMAGE;
			t.comp = xsdk_xsc_operand(ins, 0);
			break;
		case SPV_TYPE_STRUCT:
			t.kind = XSC_TYPE_STRUCT;
			t.members = ops;
			t.member_count = count;
			break;
		case SPV_TYPE_POINTER:
			t.kind = XSC_TYPE_POINTER;
			t.storage_class = xsdk_xsc_operand(ins, 0);
			t.comp = xsdk_xsc_operand(ins, 1);
			break;
		case SPV_TYPE_FUNCTION:
			t.kind = XSC_TYPE_FUNCTION;
			break;
		default:
			continue;
		}
		if (ins->result_id < m->id_limit)
			types[ins->result_id] = t;
	}
	return types;
}

const XscType *xsdk_xsc_type(const XscModule *m, const XscType *types, uint64_t id)
{
	if (id >= m->id_limit || types[id].kind == XSC_TYPE_NONE)
		return NULL;
	return &types[id];
}

// A member's decoration of a kind, its first argument or fallback; -1 for a decoration with none.
static int64_t member_arg(const XscModule *m, uint32_t target, uint32_t member, uint32_t decoration, int64_t fallback)
{
	return xsdk_xsc_decoration_arg(xsdk_xsc_member_decoration(m, target, member, decoration), fallback);
}

// Bytes a block member of this type spans, by its std140 decorations; -1, with the error set, for one it can't size.
static int64_t type_bytes(
	Xsc *x, const XscModule *m, const XscType *types, uint64_t tid, int64_t member_struct, int64_t member_index)
{
	const XscType *const t = xsdk_xsc_type(m, types, tid);
	if (!t || t->kind == XSC_TYPE_FLOAT || t->kind == XSC_TYPE_INT || t->kind == XSC_TYPE_BOOL)
		return 4;
	if (t->kind == XSC_TYPE_VECTOR)
		return 4 * (int64_t)t->count;
	if (t->kind == XSC_TYPE_MATRIX) {
		const bool member = member_struct != XSC_NONE;
		const int64_t stride = member
			? member_arg(m, (uint32_t)member_struct, (uint32_t)member_index, SPV_DEC_MATRIX_STRIDE, DEFAULT_STRIDE)
			: DEFAULT_STRIDE;
		const XscType *const col = xsdk_xsc_type(m, types, t->comp);
		const int64_t rows = col && col->kind == XSC_TYPE_VECTOR ? col->count : 1;
		const bool row_major =
			member && xsdk_xsc_member_decoration(m, (uint32_t)member_struct, (uint32_t)member_index, SPV_DEC_ROW_MAJOR);
		return stride * (row_major ? rows : t->count);
	}
	if (t->kind == XSC_TYPE_ARRAY) {
		const int64_t stride =
			xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, (uint32_t)tid, SPV_DEC_ARRAY_STRIDE), DEFAULT_STRIDE);
		const XscInstr *const lc = xsdk_xsc_by_result(m, t->count);
		const int64_t length = lc && lc->opcode == SPV_CONSTANT ? xsdk_xsc_operand(lc, 0) : 1;
		return stride * length;
	}
	if (t->kind == XSC_TYPE_STRUCT) {
		int64_t most = 0;
		for (uint32_t i = 0; i < t->member_count; i++) {
			const int64_t offset = member_arg(m, (uint32_t)tid, i, SPV_DEC_OFFSET, 0);
			const int64_t bytes = type_bytes(x, m, types, t->members[i], (int64_t)tid, i);
			if (bytes < 0)
				return -1;
			if (i == 0 || offset + bytes > most)
				most = offset + bytes;
		}
		return most;
	}
	xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "block member of type %s", xsdk_xsc_type_kind_name(t->kind));
	return -1;
}

static bool set_role(Xsc *x, XscInterface *si, uint32_t id, XscRole role)
{
	if (id >= si->role_limit)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "variable %%%u past the module's ids", (unsigned)id);
	si->roles[id] = role;
	return true;
}

static bool add_use(Xsc *x, XscVarUse **list, size_t *count, size_t *capacity, XscVarUse use)
{
	if (!xsdk_xsc_room(x, list, *count, capacity, sizeof **list))
		return false;
	(*list)[(*count)++] = use;
	return true;
}

static bool add_constant(Xsc *x, XscInterface *si, size_t *capacity, XscConstantSlot slot)
{
	if (!xsdk_xsc_room(x, &si->constants, si->constant_count, capacity, sizeof *si->constants))
		return false;
	si->constants[si->constant_count++] = slot;
	return true;
}

static bool add_sampler_dim(Xsc *x, XscInterface *si, size_t *capacity, int64_t slot, int64_t dim)
{
	if (!xsdk_xsc_room(x, &si->sampler_dims, si->sampler_dim_count, capacity, 2 * sizeof *si->sampler_dims))
		return false;
	// Two entries per pair: grown in pairs, counted in pairs.
	si->sampler_dims[2 * si->sampler_dim_count] = slot;
	si->sampler_dims[2 * si->sampler_dim_count + 1] = dim;
	si->sampler_dim_count++;
	return true;
}

bool xsdk_xsc_sampler_dim(const XscInterface *si, uint32_t slot, int64_t *dim)
{
	bool found = false;
	for (size_t i = 0; i < si->sampler_dim_count; i++) {
		if (si->sampler_dims[2 * i] == slot) {
			*dim = si->sampler_dims[2 * i + 1];
			found = true;
		}
	}
	return found;
}

const XscRole *xsdk_xsc_role(const XscInterface *si, uint32_t id)
{
	static const XscRole none = { XSC_ROLE_NONE, 0, 0, 0 };
	return id < si->role_limit ? &si->roles[id] : &none;
}

static uint32_t usage_for_vector(const XscModule *m, const XscType *types, int64_t pointee)
{
	const XscType *const shape = pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)pointee);
	return shape && shape->kind == XSC_TYPE_VECTOR && shape->count == 2 ? XSC_USAGE_TEXCOORD : XSC_USAGE_COLOR;
}

static uint32_t location_or_zero(const XscVar *v)
{
	return v->location == XSC_NONE ? 0 : (uint32_t)v->location;
}

enum {
	SEMANTIC_BYTES = 32,
	// A usage's index takes 4 bits of a vertex element (xsc_container.c; src/shader.c, PATCH_FIELD_MASK).
	MAX_USAGE_INDEX = 15,
};

// An HLSL semantic as glslang writes it, in capitals, and the digits that end it: COLOR is COLOR0.
typedef struct {
	char name[SEMANTIC_BYTES];
	uint32_t index;
} Semantic;

// D3D9's declaration usages, numbered as libxenon has them (libxenon:drivers/xenos/xe.h:159-172).
static const char *const usages[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
	"TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };

// The variable's semantic; false for one without, as GLSL's are.
static bool semantic_of(const XscModule *m, uint32_t id, Semantic *s)
{
	const XscDecoration *const d = xsdk_xsc_decoration(m, id, SPV_DEC_USER_SEMANTIC);
	if (!d)
		return false;
	size_t length = 0;
	for (; length < SEMANTIC_BYTES - 1 && length < 4 * (size_t)d->arg_count; length++) {
		s->name[length] = (char)(d->args[length / 4] >> 8 * (length % 4) & 0xFF);
		if (!s->name[length])
			break;
	}
	size_t digits = length;
	while (digits > 0 && s->name[digits - 1] >= '0' && s->name[digits - 1] <= '9')
		digits--;
	s->index = 0;
	for (size_t i = digits; i < length && s->index <= MAX_USAGE_INDEX; i++)
		s->index = s->index * 10 + (uint32_t)(s->name[i] - '0');
	s->name[digits] = '\0';
	return true;
}

static bool is_semantic(const Semantic *s, const char *name)
{
	return !strcmp(s->name, name) && s->index == 0;
}

static int64_t usage_of(const Semantic *s)
{
	for (size_t i = 0; i < sizeof usages / sizeof usages[0]; i++) {
		if (!strcmp(s->name, usages[i]))
			return s->index <= MAX_USAGE_INDEX ? (int64_t)i : XSC_NONE;
	}
	return XSC_NONE;
}

// An interpolator's semantic, its usage and index as its record holds them (xsc_private.h, XSC_SEMANTIC); refused where
// it isn't one of D3D9's usages.
static bool interpolator_semantic(Xsc *x, const XscVar *v, const Semantic *s, uint32_t *semantic)
{
	const int64_t usage = usage_of(s);
	if (usage == XSC_NONE)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED,
			"interpolator %s's semantic %s%u: not one of D3D9's declaration usages at an index to %u", v->name, s->name,
			(unsigned)s->index, (unsigned)MAX_USAGE_INDEX);
	*semantic = XSC_SEMANTIC | (uint32_t)usage << XSC_SEMANTIC_USAGE_SHIFT | s->index;
	return true;
}

// A stage's interpolators with semantics, sorted by them, take interpolators from 0 in that order, so it declares only
// those it reads or writes; xsdk links the two stages by semantic when a title pairs them (src/shader.c,
// xsdk_shader_link). One with a semantic beside one without, or two of one semantic, is refused.
static bool pack_semantics(Xsc *x, XscInterface *si, XscVarUse *uses, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const bool semantic = uses[i].b & XSC_SEMANTIC;
		if (semantic != ((uses[0].b & XSC_SEMANTIC) != 0))
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "%s and %s: an interpolator without a semantic beside one with",
				uses[0].var->name, uses[i].var->name);
		if (!semantic)
			continue;
		if (i && uses[i].b == uses[i - 1].b)
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "%s and %s share semantic %s%u", uses[i - 1].var->name,
				uses[i].var->name, usages[uses[i].b >> XSC_SEMANTIC_USAGE_SHIFT & XSC_SEMANTIC_FIELD_MASK],
				(unsigned)(uses[i].b & XSC_SEMANTIC_FIELD_MASK));
		if (i >= XSDK_XSC_INTERPOLATORS)
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "%s: more than %u interpolators", uses[i].var->name,
				(unsigned)XSDK_XSC_INTERPOLATORS);
		uses[i].a = (uint32_t)i;
		si->roles[uses[i].var->vid].a = (int64_t)i;
	}
	return true;
}

// A varying's interpolators, a semantic's one, or a matrix's one a column at the semantic's index and those after it,
// as a location a column (Vulkan 1.0, "Location Assignment") and glslang gives HLSL's row c column c: so TEXCOORD2's
// float3x3 is TEXCOORD2 to 4.
static bool add_varying(Xsc *x, const XscModule *m, const XscType *types, XscVarUse **list, size_t *count,
	size_t *capacity, const XscVar *v, uint32_t r, uint32_t semantic)
{
	const XscType *const t = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
	const uint32_t columns = t && t->kind == XSC_TYPE_MATRIX && (semantic & XSC_SEMANTIC) ? t->count : 1;
	if (columns > 1 && (semantic & XSC_SEMANTIC_FIELD_MASK) + columns - 1 > MAX_USAGE_INDEX)
		return xsdk_xsc_fail(
			x, XSDK_XSC_REFUSED, "matrix %s's columns past semantic index %u", v->name, (unsigned)MAX_USAGE_INDEX);
	for (uint32_t c = 0; c < columns; c++) {
		if (!add_use(x, list, count, capacity, (XscVarUse){ v, r + c, semantic + c }))
			return false;
	}
	return true;
}

typedef struct {
	size_t vfetch;
	size_t interp_out;
	size_t interp_in;
	size_t color_out;
	size_t samplers;
	size_t constants;
	size_t blocks;
	size_t sampler_dims;
	size_t pending;
	size_t vulkan_samplers;
	size_t comparisons;
} ListCapacities;

typedef struct {
	bool push;
	const XscVar *var;
} PendingBlock;

static int64_t round_up(int64_t reg, int64_t group)
{
	return (reg + group - 1) / group * group;
}

static bool block_before(const PendingBlock *a, const PendingBlock *b)
{
	const int64_t a_key[3] = { !a->push, a->var->descriptor_set == XSC_NONE ? 0 : a->var->descriptor_set,
		a->var->binding == XSC_NONE ? 0 : a->var->binding };
	const int64_t b_key[3] = { !b->push, b->var->descriptor_set == XSC_NONE ? 0 : b->var->descriptor_set,
		b->var->binding == XSC_NONE ? 0 : b->var->binding };
	for (int i = 0; i < 3; i++) {
		if (a_key[i] != b_key[i])
			return a_key[i] < b_key[i];
	}
	return false;
}

// Registers for each block, push constants first, then (set, binding); each uniform block has whole groups of 4 to
// itself (resolve.py _assign_blocks).
static bool assign_blocks(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, PendingBlock *pending,
	size_t pending_count, int64_t first_reg, ListCapacities *capacity, int64_t *end)
{
	// Python's sort is stable: an insertion sort keeps equal keys in their order.
	for (size_t i = 1; i < pending_count; i++) {
		const PendingBlock moving = pending[i];
		size_t j = i;
		for (; j > 0 && block_before(&moving, &pending[j - 1]); j--)
			pending[j] = pending[j - 1];
		pending[j] = moving;
	}
	int64_t reg = first_reg;
	for (size_t b = 0; b < pending_count; b++) {
		const XscVar *const v = pending[b].var;
		if (!pending[b].push)
			reg = round_up(reg, CONSTANT_GROUP);
		const int64_t bytes = type_bytes(x, m, types, (uint64_t)v->pointee, XSC_NONE, 0);
		if (bytes < 0)
			return false;
		const int64_t regs = (bytes + 15) / 16;
		if (!set_role(x, si, v->vid, (XscRole){ XSC_ROLE_BLOCK, (int64_t)si->block_count, 0, 0 }))
			return false;
		if (!xsdk_xsc_room(x, &si->blocks, si->block_count, &capacity->blocks, sizeof *si->blocks))
			return false;
		si->blocks[si->block_count++] = (XscBlock){ .push = pending[b].push,
			.var = v,
			.struct_type = (uint32_t)v->pointee,
			.reg = (uint32_t)reg,
			.regs = (uint32_t)regs };
		const XscType *const st = xsdk_xsc_type(m, types, (uint64_t)v->pointee);
		for (uint32_t mi = 0; st && mi < st->member_count; mi++) {
			const int64_t off = member_arg(m, (uint32_t)v->pointee, mi, SPV_DEC_OFFSET, 16 * (int64_t)mi);
			const int64_t member_bytes = type_bytes(x, m, types, st->members[mi], v->pointee, mi);
			if (member_bytes < 0)
				return false;
			const int64_t mregs = (member_bytes + off % 16 + 15) / 16;
			const char *name = xsdk_xsc_member_name(m, (uint32_t)v->pointee, mi);
			if (!name) {
				char text[NAME_BYTES];
				const int length = snprintf(text, sizeof text, "m%u", (unsigned)mi);
				name = xsdk_xsc_copy_text(x, text, (size_t)length);
				if (!name)
					return false;
			}
			const XscConstantSlot slot = { name, (uint32_t)(reg + off / 16), (uint32_t)mregs, (int32_t)mi,
				(int32_t)si->block_count - 1, false };
			if (!add_constant(x, si, &capacity->constants, slot))
				return false;
		}
		reg += regs;
		if (!pending[b].push)
			reg = round_up(reg, CONSTANT_GROUP);
	}
	*end = reg;
	return true;
}

static bool read_vars(Xsc *x, const XscModule *m, const XscType *types, XscVar **vars, size_t *var_count)
{
	size_t capacity = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_VARIABLE)
			continue;
		const XscType *const ptr = xsdk_xsc_type(m, types, ins->result_type);
		const uint32_t id = ins->result_id;
		if (!xsdk_xsc_room(x, vars, *var_count, &capacity, sizeof **vars))
			return false;
		(*vars)[(*var_count)++] = (XscVar){ .vid = id,
			.name = xsdk_xsc_name(m, id, ""),
			.storage_class = xsdk_xsc_operand(ins, 0),
			.pointee = ptr ? (int64_t)ptr->comp : XSC_NONE,
			.location = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_LOCATION), XSC_NONE),
			.binding = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_BINDING), XSC_NONE),
			.builtin = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_BUILTIN), XSC_NONE),
			.descriptor_set = xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, id, SPV_DEC_DESCRIPTOR_SET), XSC_NONE) };
	}
	return true;
}

// A vertex input's usage and index: its semantic's, or from its location, 0 POSITION, 1 COLOR, then TEXCOORD.
static bool vertex_usage(Xsc *x, const XscModule *m, const XscVar *v, XscVarUse *use)
{
	Semantic s;
	if (!semantic_of(m, v->vid, &s)) {
		const uint32_t loc = location_or_zero(v);
		const uint32_t usage = loc == 0 ? XSC_USAGE_POSITION : loc == 1 ? XSC_USAGE_COLOR : XSC_USAGE_TEXCOORD;
		*use = (XscVarUse){ v, usage, loc > 2 ? loc - 2 : 0 };
		return true;
	}
	const int64_t usage = usage_of(&s);
	if (usage == XSC_NONE)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED,
			"vertex input %s's semantic %s%u: not one of D3D9's declaration usages at an index to %u", v->name, s.name,
			(unsigned)s.index, (unsigned)MAX_USAGE_INDEX);
	*use = (XscVarUse){ v, (uint32_t)usage, s.index };
	return true;
}

static bool resolve_input(
	Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, const XscVar *v, ListCapacities *capacity)
{
	if (v->builtin == BUILTIN_VERTEX_ID || v->builtin == BUILTIN_INSTANCE_ID)
		return true;
	if (!si->pixel) {
		XscVarUse use = { .var = v };
		if (!vertex_usage(x, m, v, &use) || !add_use(x, &si->vfetch, &si->vfetch_count, &capacity->vfetch, use))
			return false;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_VFETCH, (int64_t)si->vfetch_count - 1, 0, 0 });
	}
	Semantic s;
	const bool hlsl = v->builtin == XSC_NONE && semantic_of(m, v->vid, &s);
	if (v->builtin == BUILTIN_FRAG_COORD) {
		si->frag_coord = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_FRAG_COORD, 0, 0, 0 });
	}
	// D3D9's VPOS and VFACE come from the pixel parameters, as gl_FragCoord and gl_FrontFacing do
	// (xsc_lower_fragment.c).
	if (hlsl && is_semantic(&s, "VPOS")) {
		si->vpos = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_FRAG_COORD, 0, 0, 0 });
	}
	if (hlsl && is_semantic(&s, "VFACE")) {
		si->vface = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_FRONT_FACING, 0, 0, 0 });
	}
	if (v->builtin == BUILTIN_FRONT_FACING) {
		si->front_facing = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_FRONT_FACING, 0, 0, 0 });
	}
	// gl_PointCoord shares gl_FragCoord's pixel parameters (xsc_lower_fragment.c).
	if (v->builtin == BUILTIN_POINT_COORD) {
		si->point_coord = v;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_POINT_COORD, 0, 0, 0 });
	}
	// A semantic sorts by itself until pack_semantics numbers them.
	uint32_t semantic = usage_for_vector(m, types, v->pointee);
	if (hlsl && !interpolator_semantic(x, v, &s, &semantic))
		return false;
	const uint32_t r = hlsl ? semantic : location_or_zero(v);
	if (!add_varying(x, m, types, &si->interp_in, &si->interp_in_count, &capacity->interp_in, v, r, semantic))
		return false;
	return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_INTERP_IN, r, 0, 0 });
}

static bool resolve_output(
	Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, const XscVar *v, ListCapacities *capacity)
{
	const XscType *const st = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
	if (st && st->kind == XSC_TYPE_STRUCT) {
		// gl_PerVertex: the member decorated BuiltIn Position is gl_Position.
		for (uint32_t mi = 0; mi < st->member_count; mi++) {
			for (size_t i = 0; i < m->member_decoration_count; i++) {
				const XscDecoration *const d = &m->member_decorations[i];
				if (d->target != (uint32_t)v->pointee || d->member != mi || d->decoration != SPV_DEC_BUILTIN ||
					!d->arg_count || d->args[0] != BUILTIN_POSITION)
					continue;
				si->position_out = v->vid;
				if (!set_role(x, si, v->vid, (XscRole){ XSC_ROLE_POSITION_STRUCT, mi, 0, 0 }))
					return false;
			}
		}
		return true;
	}
	if (v->builtin == BUILTIN_POSITION) {
		si->position_out = v->vid;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_POSITION_STRUCT, XSC_NONE, 0, 0 });
	}
	if (v->builtin == BUILTIN_FRAG_DEPTH && si->pixel)
		si->frag_depth = v;
	Semantic s;
	const bool hlsl = semantic_of(m, v->vid, &s);
	// A PSIZE out parameter, which glslang makes a PointSize of its own, outside gl_PerVertex.
	if (hlsl && !si->pixel && v->builtin == BUILTIN_POINT_SIZE)
		si->point_size = v;
	if (v->builtin != XSC_NONE)
		return true;
	// D3D9's POSITION, PSIZE and DEPTH, which glslang makes built-ins only as out parameters.
	if (hlsl && !si->pixel && is_semantic(&s, "POSITION")) {
		si->position_out = v->vid;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_POSITION_STRUCT, XSC_NONE, 0, 0 });
	}
	if (hlsl && !si->pixel && is_semantic(&s, "PSIZE")) {
		si->point_size = v;
		return true;
	}
	if (hlsl && si->pixel && is_semantic(&s, "DEPTH")) {
		si->frag_depth = v;
		return true;
	}
	uint32_t loc = location_or_zero(v);
	if (!si->pixel) {
		uint32_t semantic = usage_for_vector(m, types, v->pointee);
		if (hlsl && !interpolator_semantic(x, v, &s, &semantic))
			return false;
		loc = hlsl ? semantic : loc;
		if (!add_varying(x, m, types, &si->interp_out, &si->interp_out_count, &capacity->interp_out, v, loc, semantic))
			return false;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_INTERP_OUT, loc, 0, 0 });
	}
	// COLORn is target n; SV_Target's location is glslang's.
	if (hlsl && !strcmp(s.name, "COLOR"))
		loc = s.index;
	if (!add_use(x, &si->color_out, &si->color_out_count, &capacity->color_out, (XscVarUse){ v, loc, 0 }))
		return false;
	return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_COLOR, loc, 0, 0 });
}

static const XscType *image_of(const XscModule *m, const XscType *types, const XscType *st)
{
	const XscType *const img = st->kind == XSC_TYPE_SAMPLED_IMAGE ? xsdk_xsc_type(m, types, st->comp) : st;
	return img && img->kind == XSC_TYPE_IMAGE ? img : NULL;
}

static int64_t image_dim(const XscModule *m, const XscType *types, const XscType *st)
{
	const XscType *const img = image_of(m, types, st);
	return img ? img->dim : XSC_NONE;
}

static bool resolve_uniform_constant(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si,
	const XscVar *v, int64_t *next_const, const XscVar ***vulkan_samplers, size_t *vulkan_count,
	ListCapacities *capacity)
{
	const XscType *const st = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
	if (!st)
		return true;
	const bool image = st->kind == XSC_TYPE_SAMPLED_IMAGE || st->kind == XSC_TYPE_IMAGE || st->kind == XSC_TYPE_SAMPLER;
	if (image && v->descriptor_set != XSC_NONE) {
		if (!xsdk_xsc_room(x, vulkan_samplers, *vulkan_count, &capacity->vulkan_samplers, sizeof **vulkan_samplers))
			return false;
		(*vulkan_samplers)[(*vulkan_count)++] = v;
		return true;
	}
	if (image) {
		const int64_t slot = v->binding != XSC_NONE ? v->binding : (int64_t)si->sampler_count;
		const int64_t dim = image_dim(m, types, st);
		if (!add_use(x, &si->samplers, &si->sampler_count, &capacity->samplers, (XscVarUse){ v, (uint32_t)slot, 0 }))
			return false;
		if (!add_sampler_dim(x, si, &capacity->sampler_dims, slot, dim))
			return false;
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_SAMPLER, slot, dim, XSC_NONE });
	}
	if (st->kind != XSC_TYPE_MATRIX && st->kind != XSC_TYPE_VECTOR && st->kind != XSC_TYPE_FLOAT &&
		st->kind != XSC_TYPE_ARRAY)
		return true;
	int64_t regs = st->kind == XSC_TYPE_MATRIX ? st->count : 1;
	int64_t elem_regs = 1;
	if (st->kind == XSC_TYPE_ARRAY) {
		// A uniform array: N contiguous registers, element i at base + i.
		const XscType *const elem = xsdk_xsc_type(m, types, st->comp);
		elem_regs = elem && elem->kind == XSC_TYPE_MATRIX ? elem->count : 1;
		const XscInstr *const lc = xsdk_xsc_by_result(m, st->count);
		const int64_t length = lc && lc->opcode == SPV_CONSTANT ? xsdk_xsc_operand(lc, 0) : 1;
		regs = (length > 1 ? length : 1) * elem_regs;
	}
	const int64_t reg = v->location != XSC_NONE ? v->location : *next_const;
	if (reg + regs > *next_const)
		*next_const = reg + regs;
	const char *name = v->name;
	if (!name[0]) {
		char text[NAME_BYTES];
		const int length = snprintf(text, sizeof text, "u%lld", (long long)reg);
		name = xsdk_xsc_copy_text(x, text, (size_t)length);
		if (!name)
			return false;
	}
	if (!add_constant(
			x, si, &capacity->constants, (XscConstantSlot){ name, (uint32_t)reg, (uint32_t)regs, -1, -1, false }))
		return false;
	if (st->kind == XSC_TYPE_ARRAY)
		return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_ARRAY, reg, regs, elem_regs });
	return set_role(x, si, v->vid, (XscRole){ XSC_ROLE_CONSTANT, reg, regs, 0 });
}

static void sort_uses(XscVarUse *uses, size_t count, bool by_location)
{
	for (size_t i = 1; i < count; i++) {
		const XscVarUse moving = uses[i];
		const int64_t key = by_location ? location_or_zero(moving.var) : moving.a;
		size_t j = i;
		for (; j > 0; j--) {
			const int64_t before = by_location ? location_or_zero(uses[j - 1].var) : uses[j - 1].a;
			if (before <= key)
				break;
			uses[j] = uses[j - 1];
		}
		uses[j] = moving;
	}
}

static bool vulkan_sampler_before(const XscVar *a, const XscVar *b)
{
	if (a->descriptor_set != b->descriptor_set)
		return a->descriptor_set < b->descriptor_set;
	return (a->binding == XSC_NONE ? 0 : a->binding) < (b->binding == XSC_NONE ? 0 : b->binding);
}

static uint32_t binding_of(const XscVar *v)
{
	return v->binding == XSC_NONE ? 0 : (uint32_t)v->binding;
}

// HLSL's register(sN) is binding N and a sampler without one binding 0 (glslang:glslang/HLSL/hlslParseHelper.cpp:
// 6402-6412), so each takes its binding as its fetch constant, as D3D9's SetTexture(N) binds it; one whose binding a
// sampler before it took, as a sampler without a register beside one at s0 or another without, takes the lowest no
// binding names.
static bool hlsl_slots(Xsc *x, const XscVar **samplers, size_t count, int64_t *slots)
{
	uint32_t taken = 0;
	for (size_t i = 0; i < count; i++) {
		const uint32_t b = binding_of(samplers[i]);
		if (b >= TEXTURE_SLOTS)
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "sampler %s at s%u, past the %d fetch constants",
				samplers[i]->name, (unsigned)b, TEXTURE_SLOTS);
		slots[i] = taken >> b & 1 ? XSC_NONE : (int64_t)b;
		taken |= 1u << b;
	}
	for (size_t i = 0; i < count; i++) {
		uint32_t s = 0;
		while (slots[i] == XSC_NONE && s < TEXTURE_SLOTS && taken >> s & 1)
			s++;
		if (s == TEXTURE_SLOTS)
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "more samplers than the %d fetch constants", TEXTURE_SLOTS);
		if (slots[i] == XSC_NONE) {
			slots[i] = s;
			taken |= 1u << s;
		}
	}
	return true;
}

static bool add_vulkan_samplers(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si,
	const XscVar **samplers, size_t count, ListCapacities *capacity)
{
	for (size_t i = 1; i < count; i++) {
		const XscVar *const moving = samplers[i];
		size_t j = i;
		for (; j > 0 && vulkan_sampler_before(moving, samplers[j - 1]); j--)
			samplers[j] = samplers[j - 1];
		samplers[j] = moving;
	}
	int64_t *const slots = xsdk_xsc_alloc(x, (count + 1) * sizeof *slots);
	if (!slots)
		return false;
	for (size_t i = 0; i < count; i++)
		slots[i] = (int64_t)i;
	if (xsdk_xsc_hlsl(m) && !hlsl_slots(x, samplers, count, slots))
		return false;
	for (size_t i = 0; i < count; i++) {
		const XscVar *const v = samplers[i];
		const XscType *const st = xsdk_xsc_type(m, types, (uint64_t)v->pointee);
		const int64_t dim = image_dim(m, types, st);
		if (!add_use(
				x, &si->samplers, &si->sampler_count, &capacity->samplers, (XscVarUse){ v, (uint32_t)slots[i], 0 }))
			return false;
		if (!add_sampler_dim(x, si, &capacity->sampler_dims, slots[i], dim))
			return false;
		if (!set_role(x, si, v->vid, (XscRole){ XSC_ROLE_SAMPLER, slots[i], dim, XSC_NONE }))
			return false;
	}
	return true;
}

// A register past the blocks for each sampler of a depth image, holding its compare op's weights, which the runtime
// writes from the sampler it binds: the comparison runs after the fetch.
static bool assign_comparisons(
	Xsc *x, const XscModule *m, const XscType *types, XscInterface *si, ListCapacities *capacity)
{
	for (size_t i = 0; i < si->sampler_count; i++) {
		const XscVarUse *const use = &si->samplers[i];
		const XscType *const st = xsdk_xsc_type(m, types, (uint64_t)use->var->pointee);
		const XscType *const img = st ? image_of(m, types, st) : NULL;
		if (!img || !img->depth)
			continue;
		const uint32_t reg = si->constant_end++;
		char text[NAME_BYTES];
		const int length = snprintf(text, sizeof text, "_compare%u", (unsigned)use->a);
		const char *const name = xsdk_xsc_copy_text(x, text, (size_t)length);
		if (!name || !add_constant(x, si, &capacity->constants, (XscConstantSlot){ name, reg, 1, -1, -1, false }) ||
			!add_use(x, &si->comparisons, &si->comparison_count, &capacity->comparisons,
				(XscVarUse){ use->var, reg, use->a }))
			return false;
		si->roles[use->var->vid].c = reg;
	}
	return true;
}

// Whether a block member is an int or a bool, which glslang makes an unsigned int, or a vector of them.
static bool integral(const XscModule *m, const XscType *types, const XscInterface *si, const XscConstantSlot *c)
{
	const XscType *const st = xsdk_xsc_type(m, types, si->blocks[c->block].struct_type);
	const XscType *t =
		st && (uint32_t)c->member < st->member_count ? xsdk_xsc_type(m, types, st->members[c->member]) : NULL;
	t = t && t->kind == XSC_TYPE_VECTOR ? xsdk_xsc_type(m, types, t->comp) : t;
	return t && (t->kind == XSC_TYPE_INT || t->kind == XSC_TYPE_BOOL);
}

// An HLSL bool or int glslang packed into a register another member of its block holds, as i10's b0 and i0 over c0's
// array, takes a register of its own past the blocks, as glslang drops the b# and i# that D3D9 keeps apart
// (glslang:glslang/HLSL/hlslParseHelper.cpp:6396-6431): the float D3D9 sets for it lands on no other constant. One
// alone in its register stays there, as a register(cN) puts it.
static bool move_shared(Xsc *x, const XscModule *m, const XscType *types, XscInterface *si)
{
	bool *const shared = xsdk_xsc_alloc(x, si->constant_count + 1);
	if (!shared)
		return false;
	for (size_t i = 0; i < si->constant_count; i++) {
		const XscConstantSlot *const c = &si->constants[i];
		for (size_t j = 0; c->block >= 0 && j < si->constant_count; j++) {
			const XscConstantSlot *const o = &si->constants[j];
			shared[i] =
				shared[i] || (j != i && o->block == c->block && o->reg < c->reg + c->regs && c->reg < o->reg + o->regs);
		}
	}
	for (size_t i = 0; i < si->constant_count; i++) {
		XscConstantSlot *const c = &si->constants[i];
		if (!shared[i] || !integral(m, types, si, c))
			continue;
		c->reg = si->constant_end++;
		c->regs = 1;
		c->moved = true;
	}
	return true;
}

enum {
	// Vulkan's gl_InstanceIndex, firstInstance included.
	BUILTIN_INSTANCE_INDEX = 43,
};

// The instance group of a vertex shader reading gl_InstanceIndex takes the register past the blocks, so it shares no
// group of 4 with a uniform block the GPU may load, as a group loaded mustn't also be set (include/xsdk/gpu.h,
// xsdk_gpu_load_vertex_constants).
static bool assign_instance_index(Xsc *x, XscInterface *si, const XscVar *vars, size_t var_count, int64_t *end)
{
	for (size_t i = 0; i < var_count && !si->pixel; i++) {
		if (vars[i].storage_class != SC_INPUT || vars[i].builtin != BUILTIN_INSTANCE_INDEX)
			continue;
		if (!set_role(x, si, vars[i].vid, (XscRole){ XSC_ROLE_INSTANCE_INDEX, *end, 0, 0 }))
			return false;
		(*end)++;
	}
	return true;
}

int64_t xsdk_xsc_instance_index_reg(const XscInterface *si)
{
	for (uint32_t id = 0; id < si->role_limit; id++) {
		if (si->roles[id].kind == XSC_ROLE_INSTANCE_INDEX)
			return si->roles[id].a;
	}
	return XSC_NONE;
}
bool xsdk_xsc_resolve_interface(Xsc *x, const XscModule *m, XscInterface *si)
{
	memset(si, 0, sizeof *si);
	if (!m->entry_point_count)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "list index out of range");
	const XscType *const types = xsdk_xsc_resolve_types(x, m);
	if (!types)
		return false;
	si->pixel = m->entry_points[0].model != 0;
	si->role_limit = m->id_limit;
	si->roles = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *si->roles);
	if (!si->roles)
		return false;
	XscVar *vars = NULL;
	size_t var_count = 0;
	if (!read_vars(x, m, types, &vars, &var_count))
		return false;
	ListCapacities capacity = { 0 };
	PendingBlock *pending = NULL;
	size_t pending_count = 0;
	const XscVar **vulkan_samplers = NULL;
	size_t vulkan_count = 0;
	int64_t next_const = 0;
	for (size_t i = 0; i < var_count; i++) {
		const XscVar *const v = &vars[i];
		bool ok = true;
		// A vertex shader's built-in isn't a stream: gl_InstanceIndex takes a register past the blocks, and loading any
		// other is refused.
		if (v->storage_class == SC_INPUT && (si->pixel || v->builtin == XSC_NONE)) {
			ok = resolve_input(x, m, types, si, v, &capacity);
		} else if (v->storage_class == SC_OUTPUT) {
			ok = resolve_output(x, m, types, si, v, &capacity);
		} else if (v->storage_class == SC_UNIFORM || v->storage_class == SC_PUSH_CONSTANT) {
			const XscType *const st = v->pointee == XSC_NONE ? NULL : xsdk_xsc_type(m, types, (uint64_t)v->pointee);
			if (st && st->kind == XSC_TYPE_STRUCT) {
				ok = xsdk_xsc_room(x, &pending, pending_count, &capacity.pending, sizeof *pending);
				if (ok)
					pending[pending_count++] = (PendingBlock){ v->storage_class == SC_PUSH_CONSTANT, v };
			}
		} else if (v->storage_class == SC_UNIFORM_CONSTANT) {
			ok = resolve_uniform_constant(x, m, types, si, v, &next_const, &vulkan_samplers, &vulkan_count, &capacity);
		}
		if (!ok)
			return false;
	}
	int64_t end;
	if (!assign_blocks(x, m, types, si, pending, pending_count, next_const, &capacity, &end) ||
		!assign_instance_index(x, si, vars, var_count, &end))
		return false;
	si->constant_end = (uint32_t)end;
	if (xsdk_xsc_hlsl(m) && !move_shared(x, m, types, si))
		return false;
	if (!add_vulkan_samplers(x, m, types, si, vulkan_samplers, vulkan_count, &capacity) ||
		!assign_comparisons(x, m, types, si, &capacity))
		return false;
	sort_uses(si->vfetch, si->vfetch_count, true);
	sort_uses(si->interp_in, si->interp_in_count, false);
	sort_uses(si->interp_out, si->interp_out_count, false);
	return pack_semantics(x, si, si->interp_in, si->interp_in_count) &&
		pack_semantics(x, si, si->interp_out, si->interp_out_count);
}
