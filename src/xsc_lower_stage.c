#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xsc_lower.h"

// What lower.py's stage functions share: the store map, the locals, the literal constants and the needs.

bool xsdk_xsc_enter(XscLower *l)
{
	if (l->depth >= XSC_DEPTH_LIMIT)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "maximum recursion depth exceeded");
	l->depth++;
	return true;
}

bool xsdk_xsc_store_map(Xsc *x, const XscModule *m, XscStores *stores)
{
	stores->value = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *stores->value);
	stores->targets = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *stores->targets);
	stores->target_count = 0;
	if (!stores->value || !stores->targets)
		return false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t target = xsdk_xsc_operand(ins, 0);
		if (ins->opcode != SPV_STORE || target >= m->id_limit)
			continue;
		if (!stores->value[target])
			stores->targets[stores->target_count++] = target;
		stores->value[target] = xsdk_xsc_operand(ins, 1);
	}
	return true;
}

bool *xsdk_xsc_local_vars(Xsc *x, const XscModule *m, size_t first, size_t end)
{
	bool *const locals = xsdk_xsc_alloc(x, m->id_limit);
	for (size_t i = 0; locals && i < m->instr_count; i++) {
		if (xsdk_xsc_declares_local(m, i, first, end) && m->instrs[i].result_id < m->id_limit)
			locals[m->instrs[i].result_id] = true;
	}
	return locals;
}

void xsdk_xsc_fill_local_store(XscLower *l, const XscStores *stores, const bool *locals)
{
	for (size_t i = 0; i < stores->target_count; i++) {
		const uint32_t target = stores->targets[i];
		if (locals[target])
			l->local_store[target] = stores->value[target];
	}
}

static uint32_t chain_base(const XscModule *m, uint32_t id)
{
	const XscInstr *const ins = xsdk_xsc_by_result(m, id);
	return ins && ins->opcode == SPV_ACCESS_CHAIN ? xsdk_xsc_operand(ins, 0) : 0;
}

// Refuses the locals the lowering would read wrong, as it reads each as its last store and the unroller left them.
bool xsdk_xsc_check_local_stores(
	Xsc *x, const XscModule *m, const bool *locals, uint32_t merged_local, uint32_t merge_label)
{
	bool *const stored = xsdk_xsc_alloc(x, m->id_limit);
	if (!stored)
		return false;
	bool merged = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t target = xsdk_xsc_operand(ins, 0);
		if (ins->opcode == SPV_LABEL && merge_label && ins->result_id == merge_label)
			merged = true;
		if (ins->opcode == SPV_LOAD && merged_local && !merged &&
			(target == merged_local || chain_base(m, target) == merged_local))
			return xsdk_xsc_fail(
				x, XSDK_XSC_REFUSED, "local %%%u read before the if/else that assigns it ends", (unsigned)merged_local);
		if (ins->opcode != SPV_STORE)
			continue;
		const uint32_t base = chain_base(m, target);
		if (base && base < m->id_limit && locals[base])
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "a store into part of local %%%u", (unsigned)base);
		if (target < m->id_limit && locals[target] && target != merged_local) {
			if (stored[target])
				return xsdk_xsc_fail(x, XSDK_XSC_REFUSED,
					"local %%%u stored more than once where the unroller doesn't rewrite it", (unsigned)target);
			stored[target] = true;
		}
	}
	return true;
}

// Literals go past the constants the shader declares.
int64_t xsdk_xsc_literal_base(const XscInterface *si)
{
	int64_t base = 0;
	for (size_t i = 0; i < si->constant_count; i++) {
		const int64_t end = (int64_t)si->constants[i].reg + si->constants[i].regs;
		if (end > base)
			base = end;
	}
	return base > si->constant_end ? base : si->constant_end;
}

const char *xsdk_xsc_format(Xsc *x, const char *format, ...)
{
	char text[XSDK_XSC_MESSAGE_BYTES];
	va_list args;
	va_start(args, format);
	(void)vsnprintf(text, sizeof text, format, args);
	va_end(args);
	return xsdk_xsc_copy_text(x, text, strlen(text));
}

XscConstant xsdk_xsc_vector_constant(const char *name, int64_t reg, int64_t regs)
{
	return (XscConstant){ name, XSC_REGISTER_SET_FLOAT4, (uint32_t)reg, (uint32_t)regs, XSC_CLASS_VECTOR,
		XSC_TYPE_FLOAT_PARAMETER, 1, 4, 0 };
}

bool xsdk_xsc_add_constant(Xsc *x, XscConstants *constants, XscConstant c)
{
	if (!xsdk_xsc_room(x, &constants->items, constants->count, &constants->capacity, sizeof *constants->items))
		return false;
	constants->items[constants->count++] = c;
	return true;
}

// ---- the constant table's entries: GLSL's as before, HLSL's what a port binds by name (docs/scope/50-hlsl.md) ----

enum {
	MATRIX_SIZE = 4,
	SAMPLER_2D_PARAMETER = 0xC,
	// SPIR-V's 1D to Cube, which sampler1D to samplerCUBE follow in the table's types (SPIR-V 1.0, section 3.8).
	SPIRV_DIM_CUBE = 3,
	// OpTypeInt's signedness; glslang makes HLSL's bool an unsigned int.
	INT_SIGNED = 1,
};

XscConstant xsdk_xsc_sampler_constant(XscLower *l, int64_t slot)
{
	XscConstant c = { NULL, XSC_REGISTER_SET_SAMPLER, (uint32_t)slot, 1, XSC_CLASS_OBJECT, SAMPLER_2D_PARAMETER, 1, 1,
		0 };
	for (size_t i = 0; xsdk_xsc_hlsl(l->m) && i < l->si->sampler_count; i++) {
		int64_t dim = XSC_NONE;
		if (l->si->samplers[i].a != slot || !l->si->samplers[i].var->name[0])
			continue;
		c.name = l->si->samplers[i].var->name;
		(void)xsdk_xsc_sampler_dim(l->si, (uint32_t)slot, &dim);
		c.param_type = dim >= 0 && dim <= SPIRV_DIM_CUBE ? XSC_TYPE_SAMPLER_1D_PARAMETER + (uint32_t)dim
														 : XSC_TYPE_SAMPLER_PARAMETER;
	}
	if (!c.name)
		c.name = xsdk_xsc_format(l->x, "tex%lld", (long long)slot);
	return c;
}

// An HLSL block member's entry as its type is: a scalar, vector or matrix, its rows and columns as HLSL's, a matrix
// D3D9 holds by columns where glslang's SPIR-V holds it by rows, and an array's elements.
static XscConstant typed_constant(const XscLower *l, const XscConstantSlot *cs)
{
	XscConstant c = xsdk_xsc_vector_constant(cs->name, cs->reg, cs->regs);
	const XscBlock *const block = &l->si->blocks[cs->block];
	const XscType *const st = xsdk_xsc_type(l->m, l->types, block->struct_type);
	uint32_t tid = st && (uint32_t)cs->member < st->member_count ? st->members[cs->member] : 0;
	const XscType *t = xsdk_xsc_type(l->m, l->types, tid);
	if (t && t->kind == XSC_TYPE_ARRAY) {
		const XscInstr *const length = xsdk_xsc_by_result(l->m, t->count);
		c.elements = length && length->opcode == SPV_CONSTANT ? xsdk_xsc_operand(length, 0) : 1;
		tid = t->comp;
		t = xsdk_xsc_type(l->m, l->types, tid);
	}
	if (!t)
		return c;
	const XscType *const part =
		t->kind == XSC_TYPE_VECTOR || t->kind == XSC_TYPE_MATRIX ? xsdk_xsc_type(l->m, l->types, t->comp) : t;
	c.rows = 1;
	c.columns = t->kind == XSC_TYPE_VECTOR ? t->count : 1;
	c.param_class = c.columns > 1 ? XSC_CLASS_VECTOR : XSC_CLASS_SCALAR;
	if (t->kind == XSC_TYPE_MATRIX) {
		// glslang gives HLSL's row r SPIR-V's column r, and column_major RowMajor
		// (glslang:glslang/HLSL/hlslParseHelper.cpp:74, 7136-7141).
		const bool by_columns =
			xsdk_xsc_member_decoration(l->m, block->struct_type, (uint32_t)cs->member, SPV_DEC_ROW_MAJOR) != NULL;
		c.param_class = by_columns ? XSC_CLASS_MATRIX_COLUMNS : XSC_CLASS_MATRIX_ROWS;
		c.rows = t->count;
		c.columns = part && part->kind == XSC_TYPE_VECTOR ? part->count : 1;
	} else if (part && part->kind == XSC_TYPE_INT) {
		const XscInstr *const it = xsdk_xsc_by_result(l->m, t->kind == XSC_TYPE_VECTOR ? t->comp : tid);
		c.param_type = it && xsdk_xsc_operand(it, 1) == INT_SIGNED ? XSC_TYPE_INT_PARAMETER : XSC_TYPE_BOOL_PARAMETER;
	}
	return c;
}

static bool integral_slot(const XscLower *l, const XscConstantSlot *cs, const XscConstant *c)
{
	return cs->block >= 0 && xsdk_xsc_hlsl(l->m) &&
		(c->param_type == XSC_TYPE_INT_PARAMETER || c->param_type == XSC_TYPE_BOOL_PARAMETER);
}

bool xsdk_xsc_uniform_constants(XscLower *l, XscConstants *constants, bool matrices)
{
	const bool hlsl = xsdk_xsc_hlsl(l->m);
	for (size_t i = 0; i < l->si->constant_count; i++) {
		const XscConstantSlot *const cs = &l->si->constants[i];
		XscConstant c = xsdk_xsc_vector_constant(cs->name, cs->reg, cs->regs);
		if (matrices && cs->regs > 1)
			c = (XscConstant){ cs->name, XSC_REGISTER_SET_FLOAT4, cs->reg, cs->regs, XSC_CLASS_MATRIX_COLUMNS,
				XSC_TYPE_FLOAT_PARAMETER, MATRIX_SIZE, MATRIX_SIZE, 0 };
		if (hlsl && cs->block >= 0)
			c = typed_constant(l, cs);
		if (integral_slot(l, cs, &c) && !l->slot_read[i])
			continue;
		if (!xsdk_xsc_add_constant(l->x, constants, c))
			return false;
	}
	for (size_t i = 0; i < l->integer_count; i++) {
		if (!xsdk_xsc_add_constant(l->x, constants, l->integers[i]))
			return false;
	}
	return true;
}

bool xsdk_xsc_integer_constant(XscLower *l, int64_t slot, uint32_t set, uint32_t index, uint32_t *found)
{
	const char *const name = l->si->constants[slot].name;
	for (size_t i = 0; i < l->integer_count; i++) {
		if (l->integers[i].register_set == set && !strcmp(l->integers[i].name, name)) {
			*found = l->integers[i].register_index;
			return true;
		}
	}
	if (!xsdk_xsc_room(l->x, &l->integers, l->integer_count, &l->integer_capacity, sizeof *l->integers))
		return false;
	const uint32_t type = set == XSC_REGISTER_SET_BOOL ? XSC_TYPE_BOOL_PARAMETER : XSC_TYPE_INT_PARAMETER;
	l->integers[l->integer_count++] = (XscConstant){ name, set, index, 1, XSC_CLASS_SCALAR, type, 1, 1, 0 };
	*found = index;
	return true;
}

bool xsdk_xsc_add_need(Xsc *x, XscLowered *out, XsdkXscNeed need)
{
	if (!xsdk_xsc_room(x, &out->needs, out->need_count, &out->need_capacity, sizeof *out->needs))
		return false;
	out->needs[out->need_count++] = need;
	return true;
}

bool xsdk_xsc_literal_constants(XscLower *l, XscConstants *constants)
{
	for (size_t i = 0; i < l->pool_count; i++) {
		const int64_t reg = l->pool[i].reg;
		const char *const name = xsdk_xsc_format(l->x, "_lit%lld", (long long)reg);
		if (!name || !xsdk_xsc_add_constant(l->x, constants, xsdk_xsc_vector_constant(name, reg, 1)))
			return false;
	}
	return true;
}

bool xsdk_xsc_literal_needs(XscLower *l, XscLowered *out)
{
	for (size_t i = 0; i < l->pool_count; i++) {
		XsdkXscNeed need = { .kind = XSDK_XSC_LITERAL, .reg = (uint32_t)l->pool[i].reg, .regs = 1 };
		memcpy(need.value, l->pool[i].value, sizeof need.value);
		if (!xsdk_xsc_add_need(l->x, out, need))
			return false;
	}
	return true;
}
