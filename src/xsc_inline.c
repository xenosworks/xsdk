#include <string.h>

#include "xsc_private.h"

// Each function call inlined, so the entry point is the module's one function, as the unroller and the lowerings take
// it. glslang leaves GLSL's helper functions as they are, where spirv-opt inlines HLSL's (glslang
// SPIRV/SpvTools.cpp:202), each argument a pointer to a variable of the caller's: the callee's body takes the call's
// place, each id of its own a new one and each parameter the argument, its variables among the entry point's. The other
// functions go. A callee may branch if it returns once, from its last block: its first block's work joins the block of
// the call, its others follow, and the caller's work after the call goes on in its last. A phi naming the block of a
// call then names the block that block now ends in. A callee that returns from more than one place is refused.

enum {
	OP_LINE = 8,
	OP_FUNCTION_PARAMETER = 55,
	OP_FUNCTION_CALL = 57,
	OP_UNREACHABLE = 255,
	OP_NO_LINE = 317,
	// GLSL has no recursion (GLSL ES 3.10, section 6.1); a call nested past this is taken for one.
	MAX_CALL_DEPTH = 16,
};

typedef struct {
	XscInstr *items;
	size_t count;
	size_t capacity;
} List;

// A block as the module has it, and the block of the output its terminator ends up in.
typedef struct {
	uint32_t block;
	uint32_t ends_in;
} Tail;

typedef struct {
	Xsc *x;
	const XscModule *m;
	uint32_t next_id;
	List out;
	// The callees' variables, which go at the top of the entry point's first block.
	List vars;
	// The block of the output being written, and the block of the module whose work it is.
	uint32_t block;
	uint32_t source_block;
	Tail *tails;
	size_t tail_count;
	size_t tail_capacity;
} Inliner;

// Whether operand i is a literal, not an id, in an opcode xost takes.
static bool literal_operand(uint32_t opcode, uint32_t i)
{
	switch (opcode) {
	case SPV_VARIABLE:
		return i == 0;
	case SPV_EXT_INST:
		return i == 1;
	case SPV_LOAD:
	case SPV_COMPOSITE_EXTRACT:
	case SPV_SELECTION_MERGE:
		return i >= 1;
	case SPV_VECTOR_SHUFFLE:
	case SPV_COMPOSITE_INSERT:
	case SPV_STORE:
	case SPV_LOOP_MERGE:
		return i >= 2;
	case SPV_BRANCH_CONDITIONAL:
		return i >= 3;
	case SPV_SWITCH:
		return i >= 2 && i % 2 == 0;
	case SPV_IMAGE_SAMPLE_IMPLICIT_LOD:
	case SPV_IMAGE_SAMPLE_EXPLICIT_LOD:
	case SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD:
	case SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD:
		return i == 2;
	case SPV_IMAGE_SAMPLE_DREF_IMPLICIT_LOD:
	case SPV_IMAGE_SAMPLE_DREF_EXPLICIT_LOD:
		return i == 3;
	default:
		return false;
	}
}

static uint32_t mapped(const Inliner *in, const uint32_t *map, uint32_t id)
{
	return id < in->m->id_limit && map[id] ? map[id] : id;
}

static bool push(Inliner *in, List *list, XscInstr ins)
{
	if (!xsdk_xsc_room(in->x, &list->items, list->count, &list->capacity, sizeof *list->items))
		return false;
	list->items[list->count++] = ins;
	return true;
}

static bool is_terminator(uint32_t opcode)
{
	return opcode == SPV_BRANCH || opcode == SPV_BRANCH_CONDITIONAL || opcode == SPV_SWITCH || opcode == SPV_KILL ||
		opcode == SPV_RETURN || opcode == SPV_RETURN_VALUE || opcode == OP_UNREACHABLE;
}

// Where the block being written ends, once its terminator is written.
static bool ended(Inliner *in)
{
	if (!xsdk_xsc_room(in->x, &in->tails, in->tail_count, &in->tail_capacity, sizeof *in->tails))
		return false;
	in->tails[in->tail_count++] = (Tail){ in->source_block, in->block };
	return true;
}

// A copy of ins with result id result_id, each operand that's an id through map.
static bool emit_copy(Inliner *in, List *list, const XscInstr *ins, const uint32_t *map, uint32_t result_id)
{
	uint32_t operand_count;
	const uint32_t *const operands = xsdk_xsc_operands(ins, &operand_count);
	const uint32_t skip = (uint32_t)(operands - ins->words);
	uint32_t *const words = xsdk_xsc_alloc(in->x, (ins->word_count ? ins->word_count : 1) * sizeof *words);
	if (!words)
		return false;
	memcpy(words, ins->words, ins->word_count * sizeof *words);
	if (skip)
		words[skip - 1] = result_id;
	for (uint32_t i = 0; i < operand_count; i++) {
		if (!literal_operand(ins->opcode, i))
			words[skip + i] = mapped(in, map, operands[i]);
	}
	XscInstr copy = *ins;
	copy.words = words;
	copy.result_id = result_id;
	return push(in, list, copy);
}

// The callee's body in place of the call; caller_map takes the call's result to the value the callee returns.
static bool inline_call(Inliner *in, const XscInstr *call, uint32_t *caller_map, unsigned depth)
{
	const XscModule *const m = in->m;
	const uint32_t callee = xsdk_xsc_operand(call, 0);
	const XscInstr *const function = xsdk_xsc_by_result(m, callee);
	if (!function || function->opcode != SPV_FUNCTION)
		return xsdk_xsc_fail(in->x, XSDK_XSC_MALFORMED, "a call to %%%u, no function", (unsigned)callee);
	if (depth >= MAX_CALL_DEPTH)
		return xsdk_xsc_fail(in->x, XSDK_XSC_REFUSED, "%s, called recursively", xsdk_xsc_name(m, callee, "a function"));
	uint32_t *const map = xsdk_xsc_alloc(in->x, m->id_limit * sizeof *map);
	if (!map)
		return false;
	// Every id of the callee's a new one first, as a branch or a phi may name a block before it.
	const size_t first = (size_t)(function - m->instrs) + 1;
	uint32_t argument = 1;
	for (size_t i = first; i < m->instr_count && m->instrs[i].opcode != SPV_FUNCTION_END; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == OP_FUNCTION_PARAMETER)
			map[ins->result_id] = mapped(in, caller_map, xsdk_xsc_operand(call, argument++));
		else if (ins->result_id && ins->result_id < m->id_limit && ins->opcode != OP_FUNCTION_CALL)
			map[ins->result_id] = in->next_id++;
	}
	const uint32_t caller_block = in->source_block;
	bool entered = false;
	uint32_t value = 0;
	for (size_t i = first; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_FUNCTION_END)
			break;
		if (ins->opcode == OP_FUNCTION_PARAMETER || ins->opcode == OP_LINE || ins->opcode == OP_NO_LINE)
			continue;
		if (ins->opcode == SPV_RETURN || ins->opcode == SPV_RETURN_VALUE) {
			if (m->instrs[i + 1].opcode != SPV_FUNCTION_END)
				return xsdk_xsc_fail(in->x, XSDK_XSC_REFUSED,
					"a call to %s, a function that returns from more than one place",
					xsdk_xsc_name(m, callee, "a function"));
			if (ins->opcode == SPV_RETURN_VALUE)
				value = mapped(in, map, xsdk_xsc_operand(ins, 0));
			continue;
		}
		if (ins->opcode == OP_FUNCTION_CALL) {
			if (!inline_call(in, ins, map, depth + 1))
				return false;
			continue;
		}
		// The first block's work goes on in the block of the call; each other is a block of its own.
		if (ins->opcode == SPV_LABEL) {
			in->source_block = map[ins->result_id];
			if (!entered) {
				entered = true;
				continue;
			}
			in->block = map[ins->result_id];
		}
		const uint32_t id = ins->result_id < m->id_limit ? map[ins->result_id] : 0;
		if (!emit_copy(in, ins->opcode == SPV_VARIABLE ? &in->vars : &in->out, ins, map, id))
			return false;
		if (is_terminator(ins->opcode) && !ended(in))
			return false;
	}
	// The caller's work after the call goes on in the callee's last block.
	in->source_block = caller_block;
	if (call->result_id < m->id_limit)
		caller_map[call->result_id] = value;
	return true;
}

static bool has_call(const XscModule *m)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == OP_FUNCTION_CALL)
			return true;
	}
	return false;
}

// The entry point's body with its calls inlined, the other functions dropped, then the callees' variables put where
// the entry point's own end.
static bool rewrite(Inliner *in, uint32_t entry, size_t *vars_at)
{
	const XscModule *const m = in->m;
	uint32_t *const map = xsdk_xsc_alloc(in->x, m->id_limit * sizeof *map);
	if (!map)
		return false;
	bool in_entry = false, in_other = false, labelled = false, placed = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_FUNCTION) {
			in_entry = ins->result_id == entry;
			in_other = !in_entry;
		}
		if (in_other) {
			in_other = ins->opcode != SPV_FUNCTION_END;
			continue;
		}
		if (!in_entry) {
			if (!push(in, &in->out, *ins))
				return false;
			continue;
		}
		if (labelled && !placed && ins->opcode != SPV_VARIABLE) {
			*vars_at = in->out.count;
			placed = true;
		}
		labelled = labelled || ins->opcode == SPV_LABEL;
		in_entry = ins->opcode != SPV_FUNCTION_END;
		if (ins->opcode == SPV_LABEL)
			in->block = in->source_block = ins->result_id;
		const bool ok = ins->opcode == OP_FUNCTION_CALL ? inline_call(in, ins, map, 0)
														: emit_copy(in, &in->out, ins, map, ins->result_id);
		if (!ok || (is_terminator(ins->opcode) && !ended(in)))
			return false;
	}
	return true;
}

// Each phi's parent blocks, the blocks of the module, as the blocks of the output they end in.
static void retarget_phis(const Inliner *in)
{
	for (size_t i = 0; i < in->out.count; i++) {
		const XscInstr *const ins = &in->out.items[i];
		if (ins->opcode != SPV_PHI)
			continue;
		uint32_t count;
		uint32_t *const operands = (uint32_t *)(uintptr_t)xsdk_xsc_operands(ins, &count);
		for (uint32_t p = 1; p < count; p += 2) {
			for (size_t t = 0; t < in->tail_count; t++) {
				if (in->tails[t].block == operands[p]) {
					operands[p] = in->tails[t].ends_in;
					break;
				}
			}
		}
	}
}

bool xsdk_xsc_inline(Xsc *x, XscModule *m)
{
	if (!m->entry_point_count || !has_call(m))
		return true;
	Inliner in = { .x = x, .m = m, .next_id = m->bound > m->id_limit ? m->bound : m->id_limit };
	size_t vars_at = 0;
	if (!rewrite(&in, m->entry_points[0].id, &vars_at))
		return false;
	retarget_phis(&in);
	const size_t count = in.out.count + in.vars.count;
	XscInstr *const instrs = xsdk_xsc_alloc(x, count * sizeof *instrs);
	if (!instrs)
		return false;
	memcpy(instrs, in.out.items, vars_at * sizeof *instrs);
	if (in.vars.count)
		memcpy(instrs + vars_at, in.vars.items, in.vars.count * sizeof *instrs);
	memcpy(instrs + vars_at + in.vars.count, in.out.items + vars_at, (in.out.count - vars_at) * sizeof *instrs);
	m->instrs = instrs;
	m->instr_count = count;
	m->bound = in.next_id;
	return xsdk_xsc_spirv_index(x, m);
}
