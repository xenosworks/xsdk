#include <string.h>

#include "xsc_lower.h"

// Control flow the unroller left as values: the selects where an if/else's arms meet (xsc_unroll.c), each a cnd on the
// comparison it branched on, or, where every vertex or pixel takes the same arm and an arm works anything out, the
// arms in execs on the predicate, so the arm not taken is skipped; and loops a block's int or data counts, as hardware
// loops.

enum {
	SC_INPUT = 1,
	SC_FUNCTION = 7,
	// Both stages read all 32 loop constants (xenia:src/xenia/gpu/ucode.h:352), so each takes half, as libxenon halves
	// the boolean constants (libxenon:drivers/xenos/xe.c:2038-2064).
	VERTEX_FIRST_LOOP = 0,
	PIXEL_FIRST_LOOP = 16,
	LOOPS_A_STAGE = 16,
	// A loop whose count is data runs the most passes a loop constant counts (xenia:src/xenia/gpu/xenos.h:1086-1097).
	FIXED_PASSES = 255,
	// libxenon gives vertex shaders the first 128 boolean constants and pixel shaders the rest
	// (libxenon:drivers/xenos/xe.c:2038-2064), as a retail pixel shader's cexece b130 reads past 128
	// (xenia:src/xenia/gpu/ucode.h:103-111).
	PIXEL_FIRST_BOOLEAN = 128,
	BOOLEANS_A_STAGE = 128,
	SPV_LOGICAL_NOT = 168,
	SPV_I_EQUAL = 170,
	SPV_I_NOT_EQUAL = 171,
	REGISTER_BYTES = 16,
	MEMBER_BYTES = 4,
	IMAGE_SAMPLE_FIRST = SPV_IMAGE_SAMPLE_IMPLICIT_LOD,
	IMAGE_SAMPLE_LAST = SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD,
};

// How a comparison selects (cnd), and kills where it holds and where it doesn't (lower.py _CMP, _KILL_TRUE,
// _KILL_FALSE).
static const XscComparison comparisons[] = {
	{ SPV_F_ORD_GREATER_THAN, XSC_VECTOR_CND_GT, false, false, XSC_VECTOR_KILL_GT, false, XSC_VECTOR_KILL_GE, true },
	{ SPV_F_ORD_LESS_THAN, XSC_VECTOR_CND_GT, true, false, XSC_VECTOR_KILL_GT, true, XSC_VECTOR_KILL_GE, false },
	{ SPV_F_ORD_GREATER_THAN_EQUAL, XSC_VECTOR_CND_GE, false, false, XSC_VECTOR_KILL_GE, false, XSC_VECTOR_KILL_GT,
		true },
	{ SPV_F_ORD_LESS_THAN_EQUAL, XSC_VECTOR_CND_GE, true, false, XSC_VECTOR_KILL_GE, true, XSC_VECTOR_KILL_GT, false },
	{ SPV_F_ORD_EQUAL, XSC_VECTOR_CND_EQ, false, false, XSC_VECTOR_KILL_EQ, false, XSC_VECTOR_KILL_NE, false },
	{ SPV_F_ORD_NOT_EQUAL, XSC_VECTOR_CND_EQ, false, true, XSC_VECTOR_KILL_NE, false, XSC_VECTOR_KILL_EQ, false },
};

const XscComparison *xsdk_xsc_comparison(uint32_t opcode)
{
	for (size_t i = 0; i < sizeof comparisons / sizeof comparisons[0]; i++) {
		if (comparisons[i].opcode == opcode)
			return &comparisons[i];
	}
	return NULL;
}

bool xsdk_xsc_is_merge(const XscLower *l, uint32_t ssa)
{
	return ssa < l->m->id_limit && l->merge_branch[ssa] >= 0;
}

// ---- what differs from one vertex or pixel to the next ----

bool xsdk_xsc_id_operand(uint32_t opcode, uint32_t i)
{
	if (opcode >= IMAGE_SAMPLE_FIRST && opcode <= IMAGE_SAMPLE_LAST)
		return i != 2;
	if (opcode == SPV_EXT_INST)
		return i != 1;
	if (opcode == SPV_VECTOR_SHUFFLE)
		return i < 2;
	if (opcode == SPV_COMPOSITE_EXTRACT)
		return i < 1;
	if (opcode == SPV_COMPOSITE_INSERT)
		return i < 2;
	return true;
}

bool xsdk_xsc_varies_itself(const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	if (op == SPV_VARIABLE) {
		const uint32_t storage = xsdk_xsc_operand(ins, 0);
		return storage == SC_INPUT || storage == SC_FUNCTION;
	}
	return (op >= IMAGE_SAMPLE_FIRST && op <= IMAGE_SAMPLE_LAST) || (op >= SPV_DPDX && op <= SPV_FWIDTH);
}

// A hardware loop's value varies where what it carries does, on entry or from a pass; its induction variable doesn't,
// but in a loop a pixel leaves on data, which counts its own passes (emit_loop).
static bool loop_value_varies(const XscLower *l, uint32_t id)
{
	if (id >= l->m->id_limit || l->loop_of[id] < 0)
		return false;
	if (l->carried_of[id] < 0)
		return l->m->loops[l->loop_of[id]].alive != 0;
	const XscCarried *const c = &l->m->carried[l->carried_of[id]];
	return (c->init < l->m->id_limit && l->varies[c->init]) || (c->next < l->m->id_limit && l->varies[c->next]);
}

// One pass over the function from its start, marking what reads what varies and what anything reads; whether it
// marked more. Variables before the function, the inputs among them, vary from the first pass.
static bool mark_varying(XscLower *l, size_t first, bool *changed)
{
	const XscModule *const m = l->m;
	*changed = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		bool varies = xsdk_xsc_varies_itself(ins) || loop_value_varies(l, ins->result_id);
		uint32_t count;
		const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
		const bool reads = i >= first && ins->opcode != SPV_VARIABLE;
		for (uint32_t k = 0; reads && k < count; k++) {
			if (!xsdk_xsc_id_operand(ins->opcode, k) || ops[k] >= m->id_limit)
				continue;
			l->used[ops[k]] = true;
			varies = varies || l->varies[ops[k]];
		}
		if (varies && ins->result_id && ins->result_id < m->id_limit && !l->varies[ins->result_id]) {
			l->varies[ins->result_id] = true;
			*changed = true;
		}
	}
	return true;
}

bool xsdk_xsc_find_varying(XscLower *l)
{
	const XscModule *const m = l->m;
	l->varies = xsdk_xsc_alloc(l->x, m->id_limit);
	l->used = xsdk_xsc_alloc(l->x, m->id_limit);
	if (!l->varies || !l->used)
		return false;
	size_t first = 0;
	while (first < m->instr_count && m->instrs[first].opcode != SPV_FUNCTION)
		first++;
	bool changed = true;
	while (changed) {
		if (!mark_varying(l, first, &changed))
			return false;
	}
	return true;
}

// ---- selects as cnds ----

static int64_t origin_swizzle(XscOrigin o)
{
	return o.comp == XSC_BROADCAST ? 0 : xsdk_xsc_bsw(o.comp);
}

// What an arm gives the select: a bool as 1 or 0, an integer as the float that holds it, or the value.
static XscOperand arm_value(XscLower *l, uint32_t ssa, XscTypeKind kind)
{
	if (kind == XSC_TYPE_BOOL)
		return xsdk_xsc_bool01(l, ssa);
	if (kind == XSC_TYPE_INT)
		return xsdk_xsc_int_operand(l, ssa);
	return xsdk_xsc_lower_operand(l, ssa);
}

// a - b, the comparison's operands in the order its table entry gives them.
static bool difference(XscLower *l, const XscInstr *cmp, const XscComparison *c, XscOperand *d)
{
	XscOrigin a = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(cmp, 0));
	if (xsc_failed(l))
		return false;
	XscOrigin b = xsdk_xsc_scalar_origin(l, xsdk_xsc_operand(cmp, 1));
	if (xsc_failed(l))
		return false;
	if (c->cnd_swap_operands) {
		const XscOrigin t = a;
		a = b;
		b = t;
	}
	const int64_t r = xsdk_xsc_fresh(l);
	*d = xsc_reg(r);
	return xsc_emit2(l, XSC_VECTOR_ADD, xsc_src(a.o, false, origin_swizzle(a)), xsc_src(b.o, true, origin_swizzle(b)),
		r, false, 0xF);
}

// The register a cnd tests, and the cnd: a - b for a comparison it takes, as the fragment lowering's if/else does;
// otherwise the bool as 1 or 0, which cndgt takes as it is. swap says the arms trade places.
static bool work_out_test(XscLower *l, uint32_t cond, XscOperand *test, int64_t *opcode, bool *swap)
{
	const XscInstr *const cmp = xsdk_xsc_lower_result(l, cond);
	if (!cmp)
		return false;
	const XscComparison *const c = xsdk_xsc_comparison(cmp->opcode);
	*swap = false;
	if (!c) {
		*test = xsdk_xsc_bool01(l, cond);
		*opcode = XSC_VECTOR_CND_GT;
		return !xsc_failed(l);
	}
	*opcode = c->cnd;
	*swap = c->cnd_swap_arms;
	return difference(l, cmp, c, test);
}

// A branch's selects share its test.
static bool merge_test(XscLower *l, uint32_t branch, XscOperand *test, int64_t *opcode, bool *swap)
{
	if (l->branch_test[branch].kind == XSC_OPERAND_NONE &&
		!work_out_test(l, l->m->branches[branch].condition, &l->branch_test[branch], &l->branch_cnd[branch],
			&l->branch_swap[branch]))
		return false;
	*test = l->branch_test[branch];
	*opcode = l->branch_cnd[branch];
	*swap = l->branch_swap[branch];
	return true;
}

static XscTypeKind merge_kind(XscLower *l, uint32_t ssa)
{
	const XscType *const type = xsdk_xsc_result_type(l, ssa);
	const XscTypeKind kind = type ? type->kind : XSC_TYPE_NONE;
	if (kind == XSC_TYPE_VECTOR) {
		const XscType *const comp = xsdk_xsc_type(l->m, l->types, type->comp);
		if (comp && comp->kind == XSC_TYPE_INT) {
			xsdk_xsc_lower_refuse(l, "an integer vector chosen by an if/else (v1)");
			return XSC_TYPE_NONE;
		}
	} else if (kind != XSC_TYPE_FLOAT && kind != XSC_TYPE_BOOL && kind != XSC_TYPE_INT) {
		xsdk_xsc_lower_refuse(l, "a %s chosen by an if/else (v1)", xsdk_xsc_type_kind_name(kind));
		return XSC_TYPE_NONE;
	}
	return kind;
}

static bool emit_cnd(XscLower *l, const XscInstr *ins, XscTypeKind kind, int64_t dst, bool export_)
{
	XscOperand test;
	int64_t opcode;
	bool swap;
	if (!merge_test(l, (uint32_t)l->merge_branch[ins->result_id], &test, &opcode, &swap))
		return false;
	const XscOperand tv = arm_value(l, xsdk_xsc_operand(ins, 1), kind);
	if (xsc_failed(l))
		return false;
	XscOperand fv = arm_value(l, xsdk_xsc_operand(ins, 2), kind);
	if (xsc_failed(l))
		return false;
	// cnd takes one constant register.
	if (tv.kind == XSC_OPERAND_CONST && fv.kind == XSC_OPERAND_CONST) {
		fv = xsdk_xsc_to_reg(l, fv);
		if (xsc_failed(l))
			return false;
	}
	const XscOperand first = swap ? fv : tv;
	const XscOperand second = swap ? tv : fv;
	return xsc_emit3(l, opcode, xsc_plain(test), xsc_plain(first), xsc_plain(second), dst, export_, 0xF);
}

// ---- arms in execs on the predicate ----

// On a condition every vertex or pixel shares, the arm not taken runs for none of them, as an exec on the predicate
// runs only where one of 64 needs it (xenia:src/xenia/gpu/ucode.h:114-124); on data both arms run where they're
// split, as selects run them, so selects stay. An arm inside one of these stays selects, as there's one predicate.
// Pixel shaders alone: a vertex shader's arms in execs on the predicate hung the GPU on the console
// (docs/HARDWARE.md, "Branches, vulkan4xenon milestone 21"). An arm holding a hardware loop stays selects, as a loop
// isn't an exec, and so does one holding a sample or derivative lowered in a pass, as no fetch goes on the predicate.
static bool predicated(const XscLower *l, uint32_t branch)
{
	const XscBranch *const b = &l->m->branches[branch];
	bool held = false;
	for (size_t i = 0; i < l->m->loop_count; i++)
		held = held || (l->m->loops[i].first >= b->first && l->m->loops[i].first < b->end);
	// A sample or derivative varies of itself.
	for (uint32_t id = b->first; !held && id < b->end; id++) {
		const XscInstr *const ins = xsc_in_pass(l, id) ? xsdk_xsc_by_result(l->m, id) : NULL;
		held = ins && xsdk_xsc_varies_itself(ins);
	}
	return l->si->pixel && b->work && !l->in_arm && !held && l->varies && b->condition < l->m->id_limit &&
		!l->varies[b->condition];
}

// What an arm leaves the lowering's caches, put back for the other arm and after both: a value an arm works out
// holds only where it ran.
static bool save_caches(XscLower *l, XscSaved *s)
{
	const size_t n = l->m->id_limit;
	const size_t branches = l->m->branch_count + 1;
	if (!s->op_cache) {
		s->op_cache = xsdk_xsc_alloc(l->x, n * sizeof *s->op_cache);
		s->matrix_cache = xsdk_xsc_alloc(l->x, n * sizeof *s->matrix_cache);
		s->local_values = xsdk_xsc_alloc(l->x, n * sizeof *s->local_values);
		s->branch_test = xsdk_xsc_alloc(l->x, branches * sizeof *s->branch_test);
		if (!s->op_cache || !s->matrix_cache || !s->local_values || !s->branch_test)
			return false;
	}
	memcpy(s->op_cache, l->op_cache, n * sizeof *s->op_cache);
	memcpy(s->matrix_cache, l->matrix_cache, n * sizeof *s->matrix_cache);
	memcpy(s->local_values, l->local_values, n * sizeof *s->local_values);
	memcpy(s->branch_test, l->branch_test, branches * sizeof *s->branch_test);
	s->a0_set = l->a0_set;
	s->a0_part = l->a0_part;
	s->a0_scale = l->a0_scale;
	s->a0_load = l->a0_load;
	s->a0_allocs = l->a0_allocs;
	return true;
}

// Whether an arm loaded a0 for another index than it held before the branch.
static bool moved_a0(const XscLower *l, const XscSaved *s)
{
	return l->a0_set != s->a0_set || l->a0_part != s->a0_part || l->a0_scale != s->a0_scale;
}

static void restore_caches(XscLower *l, const XscSaved *s)
{
	const size_t n = l->m->id_limit;
	memcpy(l->op_cache, s->op_cache, n * sizeof *s->op_cache);
	memcpy(l->matrix_cache, s->matrix_cache, n * sizeof *s->matrix_cache);
	memcpy(l->local_values, s->local_values, n * sizeof *s->local_values);
	memcpy(l->branch_test, s->branch_test, (l->m->branch_count + 1) * sizeof *s->branch_test);
	l->a0_set = s->a0_set;
	l->a0_part = s->a0_part;
	l->a0_scale = s->a0_scale;
	l->a0_load = s->a0_load;
	l->a0_allocs = s->a0_allocs;
}

// p0 from a setp on src's x.
static bool setp_on(XscLower *l, int64_t op, XscOperand src)
{
	if (src.kind != XSC_OPERAND_REG && src.kind != XSC_OPERAND_CONST)
		return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a condition held in a matrix");
	XscAlu f = xsdk_xsc_template_mov();
	f.vector_opcode = XSC_VECTOR_MAX;
	f.vector_dest = 0;
	f.vector_write_mask = 0;
	f.vector_clamp = false;
	f.scalar_opcode = op;
	f.scalar_dest = 0;
	f.scalar_write_mask = 0;
	f.scalar_clamp = false;
	f.is_export = false;
	f.src_reg[2] = src.value;
	f.src_is_temp[2] = src.kind == XSC_OPERAND_REG;
	f.src_swizzle[2] = xsdk_xsc_bsw(0);
	f.src_negate[2] = false;
	return xsdk_xsc_emit_alu(l, &f);
}

// p0 = the condition: a setp on a - b, or on the bool as 1 or 0 not being 0 (xenia:src/xenia/gpu/ucode.h:1162-1204).
static bool emit_setp(XscLower *l, uint32_t cond)
{
	const XscInstr *const cmp = xsdk_xsc_lower_result(l, cond);
	if (!cmp)
		return false;
	const XscComparison *const c = xsdk_xsc_comparison(cmp->opcode);
	XscOperand src;
	int64_t op = XSC_SCALAR_SETP_NE;
	if (c) {
		if (!difference(l, cmp, c, &src))
			return false;
		op = c->cnd == XSC_VECTOR_CND_GT  ? XSC_SCALAR_SETP_GT
			: c->cnd == XSC_VECTOR_CND_GE ? XSC_SCALAR_SETP_GE
			: c->cnd_swap_arms            ? XSC_SCALAR_SETP_NE
										  : XSC_SCALAR_SETP_EQ;
	} else {
		src = xsdk_xsc_bool01(l, cond);
		if (xsc_failed(l))
			return false;
	}
	return setp_on(l, op, src);
}

// The interface constant of the HLSL bool a load reads from a block, which glslang makes an unsigned int and tests
// against 0; -1 for any other value.
static int64_t bool_member(XscLower *l, uint32_t value)
{
	const XscInstr *const load = xsdk_xsc_by_result(l->m, value);
	const XscInstr *const type = load && load->opcode == SPV_LOAD ? xsdk_xsc_by_result(l->m, load->result_type) : NULL;
	if (!type || type->opcode != SPV_TYPE_INT || xsdk_xsc_operand(type, 1))
		return -1;
	return xsdk_xsc_member_slot(l, xsdk_xsc_operand(load, 0));
}

// Whether the stage's boolean constant (letter 'b') or loop constant ('i') index is taken: by an HLSL member's
// register(bN) or register(iN), first + N, or by a bool or loop count already lowered.
static bool taken(const XscLower *l, char letter, uint32_t first, uint32_t index)
{
	for (size_t i = 0; i < l->si->constant_count; i++) {
		const XscConstantSlot *const c = &l->si->constants[i];
		if (c->d3d9_register == letter && first + c->d3d9_index == index)
			return true;
	}
	for (size_t i = 0; letter == 'b' && i < l->integer_count; i++) {
		if (l->integers[i].register_set == XSC_REGISTER_SET_BOOL && l->integers[i].register_index == index)
			return true;
	}
	for (size_t i = 0; letter == 'i' && i < l->loop_need_count; i++) {
		if (l->loop_needs[i].reg == index)
			return true;
	}
	return false;
}

// The boolean or loop constant an HLSL bool or int at slot takes, of the stage's count from first: its register's, as
// D3D9 sets b# and i# by number; else, a slot of -1 too, the lowest none takes, XSC_NONE where none is left.
static int64_t d3d9_constant(XscLower *l, int64_t slot, char letter, uint32_t first, uint32_t count)
{
	const XscConstantSlot *const c = slot >= 0 ? &l->si->constants[slot] : NULL;
	if (c && c->d3d9_register == letter) {
		if (c->d3d9_index < count)
			return first + c->d3d9_index;
		xsdk_xsc_lower_refuse(
			l, "%s at register(%c%u), past the stage's %u", c->name, letter, (unsigned)c->d3d9_index, (unsigned)count);
		return XSC_NONE;
	}
	for (uint32_t index = first; index < first + count; index++) {
		if (!taken(l, letter, first, index))
			return index;
	}
	return XSC_NONE;
}

// An if on an HLSL bool in a pixel shader reads the bool's boolean constant, as D3D9's static branches read b#, its
// arms run where it is when and where it isn't; boolean XSC_NONE for any other condition, which a setp tests.
static bool boolean_condition(XscLower *l, uint32_t cond, int64_t *boolean, bool *when)
{
	*boolean = XSC_NONE;
	*when = true;
	const XscInstr *ins = xsdk_xsc_by_result(l->m, cond);
	for (; ins && ins->opcode == SPV_LOGICAL_NOT; ins = xsdk_xsc_by_result(l->m, xsdk_xsc_operand(ins, 0)))
		*when = !*when;
	int64_t zero = 1;
	if (l->vertex || !xsdk_xsc_hlsl(l->m) || !ins || (ins->opcode != SPV_I_NOT_EQUAL && ins->opcode != SPV_I_EQUAL) ||
		!xsdk_xsc_int_const(l, xsdk_xsc_operand(ins, 1), &zero) || zero)
		return !xsc_failed(l);
	const int64_t slot = bool_member(l, xsdk_xsc_operand(ins, 0));
	if (slot < 0)
		return true;
	*when = *when == (ins->opcode == SPV_I_NOT_EQUAL);
	const int64_t next = d3d9_constant(l, slot, 'b', PIXEL_FIRST_BOOLEAN, BOOLEANS_A_STAGE);
	uint32_t index;
	if (xsc_failed(l))
		return false;
	if (next == XSC_NONE)
		return xsdk_xsc_lower_refuse(l, "more than %d bools in ifs (v1)", BOOLEANS_A_STAGE);
	if (!xsdk_xsc_integer_constant(l, slot, XSC_REGISTER_SET_BOOL, (uint32_t)next, &index))
		return false;
	*boolean = index;
	return true;
}

static bool reached(XscLower *l, uint32_t id)
{
	if (l->walked[id])
		return true;
	if (!xsdk_xsc_room(l->x, &l->walked_ids, l->walked_count, &l->walked_capacity, sizeof *l->walked_ids))
		return false;
	l->walked[id] = true;
	l->walked_ids[l->walked_count++] = id;
	return false;
}

// Whether an id holds no value to lower: a type, a pointer, a variable or a constant.
static bool no_value(XscLower *l, const XscInstr *ins)
{
	const XscType *const t = xsdk_xsc_result_type(l, ins->result_id);
	const uint32_t op = ins->opcode;
	return !ins->result_type || (t && t->kind == XSC_TYPE_POINTER) || op == SPV_VARIABLE || op == SPV_ACCESS_CHAIN ||
		op == SPV_CONSTANT || op == SPV_CONSTANT_COMPOSITE || op == SPV_CONSTANT_TRUE || op == SPV_CONSTANT_FALSE;
}

// Whether a value's users read through it to what's under it, so lowering it alone would make an instruction they
// don't: a bool, a component or a shuffle.
static bool read_through(XscLower *l, const XscInstr *ins)
{
	const XscType *const t = xsdk_xsc_result_type(l, ins->result_id);
	return (t && t->kind == XSC_TYPE_BOOL) || ins->opcode == SPV_COMPOSITE_EXTRACT || ins->opcode == SPV_VECTOR_SHUFFLE;
}

// What an arm or a loop's pass reads that was made before it, ids first to end made inside, lowered now, where every
// vertex or pixel runs it once, rather than in each arm or pass; an earlier if/else or loop among it is laid out first.
static bool lower_before(XscLower *l, uint32_t first, uint32_t end, uint32_t id)
{
	if (id >= l->m->id_limit || reached(l, id) || xsc_failed(l))
		return !xsc_failed(l);
	const XscInstr *const ins = xsdk_xsc_by_result(l->m, id);
	if (!ins || no_value(l, ins))
		return true;
	const bool inside = id >= first && id < end;
	if (!inside && !read_through(l, ins)) {
		(void)xsdk_xsc_lower_operand(l, id);
		return !xsc_failed(l);
	}
	if (!xsdk_xsc_enter(l))
		return false;
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	bool ok = true;
	for (uint32_t k = 0; ok && k < count; k++)
		ok = !xsdk_xsc_id_operand(ins->opcode, k) || lower_before(l, first, end, ops[k]);
	l->depth--;
	return ok;
}

static bool begin_walk(XscLower *l)
{
	if (!l->walked)
		l->walked = xsdk_xsc_alloc(l->x, l->m->id_limit);
	return l->walked != NULL;
}

static bool end_walk(XscLower *l, bool ok)
{
	for (size_t i = 0; i < l->walked_count; i++)
		l->walked[l->walked_ids[i]] = false;
	l->walked_count = 0;
	return ok;
}

static bool lower_arms_inputs(XscLower *l, uint32_t branch)
{
	const XscModule *const m = l->m;
	const XscBranch *const b = &m->branches[branch];
	bool ok = begin_walk(l);
	for (size_t i = 0; ok && i < m->merge_count; i++) {
		const XscInstr *const ins = m->merges[i].branch == branch && l->used[m->merges[i].select]
			? xsdk_xsc_by_result(m, m->merges[i].select)
			: NULL;
		ok = !ins ||
			(lower_before(l, b->first, b->end, xsdk_xsc_operand(ins, 1)) &&
				lower_before(l, b->first, b->end, xsdk_xsc_operand(ins, 2)));
	}
	return end_walk(l, ok);
}

// An arm's value for one select into its register, copied where it was lowered before the branch.
static bool emit_arm(XscLower *l, uint32_t value, XscTypeKind kind, int64_t dst)
{
	const bool lowered = value < l->m->id_limit && l->op_cache[value].kind != XSC_OPERAND_NONE;
	if (kind != XSC_TYPE_BOOL && kind != XSC_TYPE_INT && !lowered)
		return xsdk_xsc_emit_value(l, value, dst, false, false);
	const XscOperand o = arm_value(l, value, kind);
	return !xsc_failed(l) && xsdk_xsc_emit_copy(l, o, dst, false, false, 0, 0xF, false) != XSC_NONE;
}

// Every select the branch makes that anything reads, each arm's values in execs of their own on the predicate, into one
// register a select; then the selects are those registers.
static bool emit_predicated(XscLower *l, uint32_t branch)
{
	const XscModule *const m = l->m;
	int64_t *const regs = xsdk_xsc_alloc(l->x, (m->merge_count + 1) * sizeof *regs);
	const uint32_t cond = m->branches[branch].condition;
	int64_t boolean;
	bool when;
	if (!regs || !lower_arms_inputs(l, branch) || !boolean_condition(l, cond, &boolean, &when) ||
		(boolean == XSC_NONE && !emit_setp(l, cond)) || !save_caches(l, &l->saved))
		return false;
	for (size_t i = 0; i < m->merge_count; i++)
		regs[i] = m->merges[i].branch == branch && l->used[m->merges[i].select] ? xsdk_xsc_fresh(l) : XSC_NONE;
	l->in_arm = true;
	bool moved = false;
	for (int arm = 0; arm < 2; arm++) {
		const uint8_t on = !arm == (boolean == XSC_NONE || when) ? XSC_PREDICATE_TRUE : XSC_PREDICATE_FALSE;
		if (!(boolean == XSC_NONE ? xsdk_xsc_program_predicate(l->x, l->p, on)
								  : xsdk_xsc_program_boolean(l->x, l->p, boolean, on)))
			return false;
		for (size_t i = 0; i < m->merge_count; i++) {
			const XscInstr *const ins = regs[i] == XSC_NONE ? NULL : xsdk_xsc_lower_result(l, m->merges[i].select);
			const XscTypeKind kind = ins ? merge_kind(l, ins->result_id) : XSC_TYPE_NONE;
			if (ins && (kind == XSC_TYPE_NONE || !emit_arm(l, xsdk_xsc_operand(ins, 1 + (uint32_t)arm), kind, regs[i])))
				return false;
		}
		moved = moved || moved_a0(l, &l->saved);
		restore_caches(l, &l->saved);
	}
	l->in_arm = false;
	// a0 holds what the arm that ran loaded, so a read at the index from before loads it again (set_a0).
	if (moved)
		l->a0_allocs = SIZE_MAX;
	if (!xsdk_xsc_program_predicate(l->x, l->p, XSC_PREDICATE_NONE))
		return false;
	for (size_t i = 0; i < m->merge_count; i++) {
		if (regs[i] != XSC_NONE)
			l->op_cache[m->merges[i].select] = xsc_reg(regs[i]);
	}
	l->branch_done[branch] = true;
	return true;
}

// The select's register, its branch's arms laid out the first time one is wanted.
static XscOperand predicated_value(XscLower *l, uint32_t ssa)
{
	const uint32_t branch = (uint32_t)l->merge_branch[ssa];
	if (!l->branch_done[branch] && !emit_predicated(l, branch))
		return (XscOperand){ 0 };
	if (l->op_cache[ssa].kind == XSC_OPERAND_NONE)
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "a select read where nothing reads it");
	return l->op_cache[ssa];
}

bool xsdk_xsc_emit_merge(XscLower *l, uint32_t ssa, int64_t dst, bool export_)
{
	const XscInstr *const ins = xsdk_xsc_lower_result(l, ssa);
	if (!ins)
		return false;
	const XscTypeKind kind = merge_kind(l, ssa);
	if (kind == XSC_TYPE_NONE)
		return false;
	if (!predicated(l, (uint32_t)l->merge_branch[ssa]))
		return emit_cnd(l, ins, kind, dst, export_);
	const XscOperand r = predicated_value(l, ssa);
	return !xsc_failed(l) && xsdk_xsc_emit_copy(l, r, dst, export_, false, 0, 0xF, false) != XSC_NONE;
}

XscOperand xsdk_xsc_merge_operand(XscLower *l, uint32_t ssa)
{
	if (predicated(l, (uint32_t)l->merge_branch[ssa]))
		return merge_kind(l, ssa) == XSC_TYPE_NONE ? (XscOperand){ 0 } : predicated_value(l, ssa);
	const int64_t r = xsdk_xsc_fresh(l);
	return xsdk_xsc_emit_merge(l, ssa, r, false) ? xsc_reg(r) : (XscOperand){ 0 };
}

// ---- hardware loops ----

// Where the count is: the block holding the int the chain reaches, at a byte offset (xsc_lower_block.c); and the
// loop's start and step, which the runtime works its passes out from.
static bool block_count(XscLower *l, const XscHardwareLoop *loop, XsdkXscNeed *need)
{
	XscBlockAccess access;
	bool found;
	if (!xsdk_xsc_block_access(l, loop->count, &access, &found))
		return false;
	const XscType *const type = found ? xsdk_xsc_type(l->m, l->types, access.tid) : NULL;
	if (!found || access.dynamic || !type || type->kind != XSC_TYPE_INT)
		return xsdk_xsc_lower_refuse(l, "a loop's count other than an int a block holds at a fixed place (v1)");
	if (xsdk_xsc_hlsl(l->m) && (loop->start || loop->step != 1))
		return xsdk_xsc_lower_refuse(
			l, "a loop an HLSL int counts from other than 0 by 1, which a title sets by name to its passes (v1)");
	const XscInstr *const chain = xsdk_xsc_by_result(l->m, loop->count);
	const XscBlock *const block = &l->si->blocks[xsdk_xsc_role(l->si, xsdk_xsc_operand(chain, 0))->a];
	*need = (XsdkXscNeed){ .kind = XSDK_XSC_BLOCK_LOOP,
		.regs = 1,
		.block = block->push ? XSDK_XSC_PUSH_CONSTANTS : XSDK_XSC_UNIFORM_BUFFER,
		.set = block->var->descriptor_set == XSC_NONE ? 0 : (uint32_t)block->var->descriptor_set,
		.binding = block->var->binding == XSC_NONE ? 0 : (uint32_t)block->var->binding,
		.offset_bytes = (uint32_t)((access.reg - block->reg) * REGISTER_BYTES + access.comp * MEMBER_BYTES),
		.start = loop->start,
		.step = loop->step };
	return true;
}

// The loop constant the loop's count sets, one per count, from the stage's first: a block's int from a start by a
// step, or the fixed passes a loop whose count is data runs.
static bool count_constant(XscLower *l, const XscHardwareLoop *loop, uint32_t *loop_constant)
{
	XsdkXscNeed need = { .kind = XSDK_XSC_FIXED_LOOP, .regs = FIXED_PASSES };
	if (loop->count && !block_count(l, loop, &need))
		return false;
	for (size_t i = 0; i < l->loop_need_count; i++) {
		const XsdkXscNeed *const n = &l->loop_needs[i];
		if (n->kind == need.kind && n->block == need.block && n->set == need.set && n->binding == need.binding &&
			n->offset_bytes == need.offset_bytes && n->start == need.start && n->step == need.step) {
			*loop_constant = n->reg;
			return true;
		}
	}
	// An HLSL int's entry in the table, D3D9's i# as the loop constant its loops read.
	const int64_t slot = loop->count && xsdk_xsc_hlsl(l->m) ? xsdk_xsc_member_slot(l, loop->count) : -1;
	const int64_t next = d3d9_constant(l, slot, 'i', l->vertex ? VERTEX_FIRST_LOOP : PIXEL_FIRST_LOOP, LOOPS_A_STAGE);
	uint32_t index;
	if (xsc_failed(l))
		return false;
	if (next == XSC_NONE)
		return xsdk_xsc_lower_refuse(l, "loops of more than %d counts (v1)", LOOPS_A_STAGE);
	if (slot >= 0 && !xsdk_xsc_integer_constant(l, slot, XSC_REGISTER_SET_INT4, (uint32_t)next, &index))
		return false;
	need.reg = *loop_constant = (uint32_t)next;
	if (!xsdk_xsc_room(l->x, &l->loop_needs, l->loop_need_count, &l->loop_need_capacity, sizeof *l->loop_needs))
		return false;
	l->loop_needs[l->loop_need_count++] = need;
	return true;
}

bool xsdk_xsc_loop_needs(XscLower *l, XscLowered *out)
{
	for (size_t i = 0; i < l->loop_need_count; i++) {
		if (!xsdk_xsc_add_need(l->x, out, l->loop_needs[i]))
			return false;
	}
	return true;
}

static XscTypeKind carried_kind(XscLower *l, uint32_t phi)
{
	const XscType *const type = xsdk_xsc_result_type(l, phi);
	const XscTypeKind kind = type ? type->kind : XSC_TYPE_NONE;
	if (kind != XSC_TYPE_FLOAT && kind != XSC_TYPE_VECTOR && kind != XSC_TYPE_INT && kind != XSC_TYPE_BOOL) {
		xsdk_xsc_lower_refuse(l, "a %s a hardware loop carries (v1)", xsdk_xsc_type_kind_name(kind));
		return XSC_TYPE_NONE;
	}
	return kind;
}

// What a pass reads that was made before the loop, lowered once before it; the loops nested in it too, what they carry
// on entry among it.
static bool lower_pass_inputs(XscLower *l, const XscHardwareLoop *loop)
{
	const XscModule *const m = l->m;
	bool ok = begin_walk(l);
	for (size_t i = 0; ok && i < m->loop_count; i++) {
		const XscHardwareLoop *const in = &m->loops[i];
		if (in->first < loop->first || in->first >= loop->end)
			continue;
		ok = !in->break_condition || lower_before(l, loop->first, loop->end, in->break_condition);
		for (size_t c = in->carried_first; ok && c < in->carried_first + in->carried_count; c++)
			ok = (in == loop || lower_before(l, loop->first, loop->end, m->carried[c].init)) &&
				lower_before(l, loop->first, loop->end, m->carried[c].next);
	}
	return end_walk(l, ok);
}

// counter = counter + 1, a float, as xost holds integers (see "scalar int -> float"); in a loop a pixel leaves on data,
// + alive instead, so a pixel that left holds the pass it broke at.
static bool count_pass(XscLower *l, int64_t counter, int64_t alive)
{
	if (alive != XSC_NONE)
		return xsc_emit2(
			l, XSC_VECTOR_ADD, xsc_temp(counter), xsc_src(xsc_reg(alive), false, xsdk_xsc_bsw(0)), counter, false, 0xF);
	const int64_t one = xsdk_xsc_literal_splat(l, 1.0);
	return one != XSC_NONE &&
		xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(counter), xsc_plain(xsc_const(one)), counter, false, 0xF);
}

// Each carried value's register from its value on entry; LOOP_START; each pass's values worked out from the registers,
// then put in them; the induction variable counted on; a break's setp; LOOP_END. The loop constant the block's int
// sets runs it the passes from the loop's start to the int by its step, or one whose count is data 255, aL from 0 by 1
// (xenia:src/xenia/gpu/xenos.h:1086-1097). A loop a pixel leaves on data sets p0 where alive is 0, and LOOP_END breaks
// once every pixel of the group has (xenia:src/xenia/gpu/ucode.h:372-373).
static bool emit_loop(XscLower *l, uint32_t index)
{
	const XscModule *const m = l->m;
	const XscHardwareLoop *const loop = &m->loops[index];
	if (l->in_arm)
		return xsdk_xsc_lower_refuse(l, "a hardware loop inside an if/else's execs (v1)");
	if (l->loop_depth == XSC_LOOP_DEPTH)
		return xsdk_xsc_lower_refuse(l, "hardware loops nested more than %d deep", XSC_LOOP_DEPTH);
	if (loop->break_condition && loop->break_condition < m->id_limit && l->varies[loop->break_condition])
		return xsdk_xsc_lower_refuse(l,
			"a break on data from a loop counted by a block: LOOP_END breaks when all 64 vertices or pixels agree "
			"(xenia:src/xenia/gpu/ucode.h:372-373) (v1)");
	uint32_t loop_constant = 0;
	const size_t count = loop->carried_count;
	int64_t *const regs = xsdk_xsc_alloc(l->x, (count + 1) * sizeof *regs);
	XscOperand *const next = xsdk_xsc_alloc(l->x, (count + 1) * sizeof *next);
	if (!regs || !next || !count_constant(l, loop, &loop_constant) || !lower_pass_inputs(l, loop))
		return false;
	for (size_t i = 0; i < count; i++) {
		const XscCarried *const c = &m->carried[loop->carried_first + i];
		const XscTypeKind kind = carried_kind(l, c->phi);
		regs[i] = xsdk_xsc_fresh(l);
		if (kind == XSC_TYPE_NONE || !emit_arm(l, c->init, kind, regs[i]))
			return false;
	}
	const bool counts = l->used[loop->counter] || l->used[loop->counter_result];
	const int64_t counter = xsdk_xsc_fresh(l);
	const int64_t zero = counts ? xsdk_xsc_literal_splat(l, 0.0) : 0;
	if (zero == XSC_NONE ||
		(counts && xsdk_xsc_emit_copy(l, xsc_const(zero), counter, false, false, 0, 0xF, false) == XSC_NONE) ||
		!save_caches(l, &l->loop_saved[l->loop_depth]) || !xsdk_xsc_program_begin_loop(l->x, l->p, loop_constant))
		return false;
	l->loop_depth++;
	l->a0_set = false;
	for (size_t i = 0; i < count; i++)
		l->op_cache[m->carried[loop->carried_first + i].phi] = xsc_reg(regs[i]);
	l->op_cache[loop->counter] = xsc_reg(counter);
	for (size_t i = 0; i < count; i++) {
		const XscCarried *const c = &m->carried[loop->carried_first + i];
		next[i] = arm_value(l, c->next, carried_kind(l, c->phi));
		if (xsc_failed(l))
			return false;
	}
	if (loop->break_condition && !emit_setp(l, loop->break_condition))
		return false;
	// The registers take their values all at once: a value another's register holds is copied out first.
	for (size_t i = 0; i < count; i++) {
		bool shared = false;
		for (size_t j = 0; j < count; j++)
			shared = shared || (j != i && next[i].kind == XSC_OPERAND_REG && next[i].value == regs[j]);
		const int64_t r = shared ? xsdk_xsc_fresh(l) : XSC_NONE;
		if (shared && xsdk_xsc_emit_copy(l, next[i], r, false, false, 0, 0xF, false) == XSC_NONE)
			return false;
		next[i] = shared ? xsc_reg(r) : next[i];
	}
	for (size_t i = 0; i < count; i++) {
		const bool same = next[i].kind == XSC_OPERAND_REG && next[i].value == regs[i];
		if (!same && xsdk_xsc_emit_copy(l, next[i], regs[i], false, false, 0, 0xF, false) == XSC_NONE)
			return false;
	}
	const int64_t alive = loop->alive ? regs[l->carried_of[loop->alive] - (int32_t)loop->carried_first] : XSC_NONE;
	const bool breaks = loop->break_condition || alive != XSC_NONE;
	if ((counts && !count_pass(l, counter, alive)) ||
		(alive != XSC_NONE && !setp_on(l, XSC_SCALAR_SETP_EQ, xsc_reg(alive))) ||
		!xsdk_xsc_program_end_loop(l->x, l->p, breaks, alive != XSC_NONE || loop->break_when))
		return false;
	restore_caches(l, &l->loop_saved[--l->loop_depth]);
	l->a0_set = false;
	for (size_t i = 0; i < count; i++)
		l->op_cache[m->carried[loop->carried_first + i].result] = xsc_reg(regs[i]);
	l->op_cache[loop->counter_result] = xsc_reg(counter);
	l->loop_done[index] = true;
	return true;
}

XscOperand xsdk_xsc_loop_operand(XscLower *l, uint32_t ssa)
{
	const int32_t index = ssa < l->m->id_limit ? l->loop_of[ssa] : -1;
	if (index < 0) {
		xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "%%%u is no loop's", (unsigned)ssa);
		return (XscOperand){ 0 };
	}
	if (!l->loop_done[index] && !emit_loop(l, (uint32_t)index))
		return (XscOperand){ 0 };
	return l->op_cache[ssa];
}
