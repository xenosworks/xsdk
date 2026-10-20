#include <string.h>

#include "xsc_private.h"

// Switches put in the shapes glslang writes for GLSL, which the unroller and the lowerings take; none is left.
// spirv-opt's merge-return wraps a function with an early return in a switch on a constant with no cases, each return
// a branch to its merge, which holds the returned value's Phi, the output stores and the return (SPIRV-Tools
// source/opt/merge_return_pass.cpp:238-290, 932-977); each branch there becomes a copy of the merge, each Phi the value
// from that block, so each early return returns where it is, as GLSL's does. Any other switch, HLSL's or GLSL's,
// becomes an if/else chain, a case after another and the default last, each if/else meeting the one before it.

enum {
	STORAGE_OUTPUT = 3,
	OP_FUNCTION_CALL = 57,
	OP_I_EQUAL = 170,
	OP_LOGICAL_OR = 166,
	OP_UNREACHABLE = 255,
	// What a wrapper's merge may hold past its Phis, copied to each block branching there.
	MAX_TAIL = 64,
	// A switch's cases with a block of their own, their literals, and the if/else and loops a case may hold open at
	// once.
	MAX_ARMS = 32,
	MAX_LITERALS = 64,
	MAX_OPEN = 16,
};

// A switch's case, or its default, last: its first block, the switch's merge for one that only breaks; the block in it
// branching to the merge, 0 for none; the block testing it, the switch's own first, and where its if/else meets the
// next's, the switch's merge first.
typedef struct {
	uint32_t root;
	uint32_t exit;
	uint32_t test;
	uint32_t meet;
} Arm;

// A case's literal, and the constant of the selector's type it's compared with: the module's, or one made.
typedef struct {
	uint32_t literal;
	uint32_t id;
	bool minted;
} Literal;

typedef struct {
	Xsc *x;
	XscModule *m;
	uint32_t next_id;
	XscInstr *out;
	size_t count;
	size_t capacity;
	// The switch's OpSelectionMerge, and its merge block from its label: a wrapper's through its OpReturn.
	size_t header;
	uint32_t merge;
	size_t label;
	size_t tail_end;
	// Any other switch's block, its selector's type, its cases then its default, the literals and the bool type, made
	// where the module has none; the merge's Phis, and each one's id where each if/else after the first meets.
	uint32_t block;
	uint32_t selector_type;
	Arm arms[MAX_ARMS + 1];
	size_t arm_count;
	Literal literals[MAX_LITERALS];
	size_t literal_count;
	uint32_t bool_type;
	bool mint_bool;
	size_t phi_count;
	uint32_t *phi_ids;
} Pass;

static bool refuse(Pass *p, const char *message)
{
	return xsdk_xsc_fail(p->x, XSDK_XSC_REFUSED, "%s", message);
}

static bool emit(Pass *p, XscInstr ins)
{
	if (!xsdk_xsc_room(p->x, &p->out, p->count, &p->capacity, sizeof *p->out))
		return false;
	p->out[p->count++] = ins;
	return true;
}

static uint32_t *new_words(Pass *p, const uint32_t *words, uint32_t count)
{
	uint32_t *const copy = xsdk_xsc_alloc(p->x, (count ? count : 1) * sizeof *copy);
	if (copy)
		memcpy(copy, words, count * sizeof *copy);
	return copy;
}

static bool emit_new(Pass *p, uint32_t opcode, uint32_t type, uint32_t id, const uint32_t *words, uint32_t count)
{
	const uint32_t *const copy = new_words(p, words, count);
	return copy && emit(p, (XscInstr){ opcode, type, id, copy, count });
}

// merge-return's wrapper: an OpSelectionMerge, then an OpSwitch on a constant with a default alone.
static bool is_wrapper(const XscModule *m, size_t i)
{
	if (i + 1 >= m->instr_count || m->instrs[i].opcode != SPV_SELECTION_MERGE)
		return false;
	const XscInstr *const sw = &m->instrs[i + 1];
	const XscInstr *const selector = sw->opcode == SPV_SWITCH ? xsdk_xsc_by_result(m, sw->words[0]) : NULL;
	return sw->word_count == 2 && selector && selector->opcode == SPV_CONSTANT;
}

// Whether a pointer reaches an output.
static bool is_output(const XscModule *m, uint32_t pointer)
{
	const XscInstr *v = xsdk_xsc_by_result(m, pointer);
	while (v && (v->opcode == SPV_ACCESS_CHAIN || v->opcode == SPV_IN_BOUNDS_ACCESS_CHAIN))
		v = xsdk_xsc_by_result(m, v->words[2]);
	return v && v->opcode == SPV_VARIABLE && v->words[2] == STORAGE_OUTPUT;
}

// The wrapper's merge: its Phis, then values and output stores alone, then its OpReturn, at most MAX_TAIL in all.
static bool find_tail(Pass *p)
{
	const XscModule *const m = p->m;
	const XscInstr *const label = xsdk_xsc_by_result(m, p->merge);
	if (!label || label->opcode != SPV_LABEL)
		return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a switch's merge %%%u, not a block", (unsigned)p->merge);
	p->label = (size_t)(label - m->instrs);
	size_t i = p->label + 1;
	while (i < m->instr_count && m->instrs[i].opcode == SPV_PHI)
		i++;
	for (; i < m->instr_count && m->instrs[i].opcode != SPV_RETURN; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const bool value = ins->result_type && ins->result_id && ins->opcode != OP_FUNCTION_CALL &&
			ins->opcode != SPV_VARIABLE && ins->opcode != SPV_PHI;
		const bool stores_output = ins->opcode == SPV_STORE && is_output(m, ins->words[0]);
		if (!value && !stores_output)
			break;
	}
	if (i == m->instr_count || m->instrs[i].opcode != SPV_RETURN || i - p->label > MAX_TAIL)
		return refuse(p, "an early return, its merge holding more than values and output stores before a return (v1)");
	p->tail_end = i + 1;
	return true;
}

static uint32_t incoming(const XscInstr *phi, uint32_t from)
{
	for (uint32_t k = 2; k + 1 < phi->word_count; k += 2) {
		if (phi->words[k + 1] == from)
			return phi->words[k];
	}
	return 0;
}

// An id the merge made, as a copy of it has it; any other as it is.
static uint32_t copied_id(const uint32_t *old_ids, const uint32_t *new_ids, size_t count, uint32_t id)
{
	for (size_t i = 0; i < count; i++) {
		if (old_ids[i] == id)
			return new_ids[i];
	}
	return id;
}

// The merge's code for a block that branched there: each Phi the value from that block, each value made anew.
static bool emit_tail(Pass *p, uint32_t from)
{
	const XscModule *const m = p->m;
	uint32_t old_ids[MAX_TAIL], new_ids[MAX_TAIL];
	size_t mapped = 0;
	for (size_t i = p->label + 1; i < p->tail_end; i++) {
		const XscInstr *const ins = &m->instrs[i];
		old_ids[mapped] = ins->result_id;
		if (ins->opcode == SPV_PHI) {
			new_ids[mapped++] = incoming(ins, from);
			if (!new_ids[mapped - 1])
				return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a Phi without a value from %%%u", (unsigned)from);
			continue;
		}
		uint32_t *const words = new_words(p, ins->words, ins->word_count);
		if (!words)
			return false;
		uint32_t count;
		const uint32_t skip = (uint32_t)(xsdk_xsc_operands(ins, &count) - ins->words);
		for (uint32_t k = skip; k < ins->word_count; k++) {
			if (xsdk_xsc_id_operand(ins->opcode, k - skip))
				words[k] = copied_id(old_ids, new_ids, mapped, words[k]);
		}
		const uint32_t id = ins->result_id ? p->next_id++ : 0;
		if (id) {
			words[1] = id;
			new_ids[mapped++] = id;
		}
		if (!emit(p, (XscInstr){ ins->opcode, ins->result_type, id, words, ins->word_count }))
			return false;
	}
	return true;
}

// A branch to the merge, the merge's code; an if/else's arm to it, a block of its own holding that code.
static bool emit_exit(Pass *p, const XscInstr *ins, uint32_t block)
{
	if (ins->opcode == SPV_BRANCH)
		return emit_tail(p, block);
	const bool first = ins->words[1] == p->merge;
	if (first == (ins->words[2] == p->merge) || !p->count || p->out[p->count - 1].opcode != SPV_SELECTION_MERGE)
		return refuse(p, "an early return reaching its merge on other than a branch or an if/else's arm (v1)");
	uint32_t *const words = new_words(p, ins->words, ins->word_count);
	if (!words)
		return false;
	const uint32_t arm = words[first ? 1 : 2] = p->next_id++;
	return emit(p, (XscInstr){ ins->opcode, 0, 0, words, ins->word_count }) &&
		emit_new(p, SPV_LABEL, 0, arm, &arm, 1) && emit_tail(p, block);
}

// Whether an instruction names the merge where it can't be rewritten: as a Phi's block, a merge or a switch's target.
static bool names_merge(const Pass *p, const XscInstr *ins)
{
	const uint32_t op = ins->opcode;
	if (op != SPV_PHI && op != SPV_LOOP_MERGE && op != SPV_SELECTION_MERGE && op != SPV_SWITCH)
		return false;
	for (uint32_t k = 0; k < ins->word_count; k++) {
		if (ins->words[k] == p->merge)
			return true;
	}
	return false;
}

// The module without the wrapper: its switch a branch to its default, its merge copied where it was branched to.
static bool unwrap(Pass *p)
{
	const XscModule *const m = p->m;
	uint32_t block = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const bool exits = (ins->opcode == SPV_BRANCH && ins->words[0] == p->merge) ||
			(ins->opcode == SPV_BRANCH_CONDITIONAL && (ins->words[1] == p->merge || ins->words[2] == p->merge));
		bool ok = true;
		if (i == p->header + 1)
			ok = emit_new(p, SPV_BRANCH, 0, 0, &ins->words[1], 1);
		else if (i == p->header || (i >= p->label && i < p->tail_end))
			continue;
		else if (exits)
			ok = emit_exit(p, ins, block);
		else if (names_merge(p, ins))
			ok = refuse(p, "an early return reaching its merge on other than a branch or an if/else's arm (v1)");
		else
			ok = emit(p, *ins);
		if (!ok)
			return false;
		block = ins->opcode == SPV_LABEL ? ins->result_id : block;
	}
	return true;
}

// ---- any other switch: an if/else chain ----

static bool ends_block(uint32_t opcode)
{
	return opcode >= SPV_BRANCH && opcode <= OP_UNREACHABLE;
}

static size_t index_of(const XscModule *m, uint32_t id)
{
	const XscInstr *const ins = xsdk_xsc_by_result(m, id);
	return ins ? (size_t)(ins - m->instrs) : m->instr_count;
}

static size_t terminator_at(const XscModule *m, size_t label)
{
	size_t i = label + 1;
	while (i < m->instr_count && !ends_block(m->instrs[i].opcode))
		i++;
	return i;
}

// A terminator's targets: a branch's, an if/else's two, or a switch's default and cases, a literal a word each.
static uint32_t targets(const XscInstr *t, uint32_t out[MAX_LITERALS + 1])
{
	if (t->opcode == SPV_BRANCH) {
		out[0] = t->words[0];
		return 1;
	}
	if (t->opcode == SPV_BRANCH_CONDITIONAL) {
		out[0] = t->words[1];
		out[1] = t->words[2];
		return 2;
	}
	uint32_t n = 0;
	for (uint32_t k = 1; t->opcode == SPV_SWITCH && k < t->word_count && n <= MAX_LITERALS; k += 2)
		out[n++] = t->words[k];
	return n;
}

static bool is_root(const Pass *p, uint32_t label)
{
	for (size_t a = 0; a <= p->arm_count; a++) {
		if (p->arms[a].root == label)
			return true;
	}
	return false;
}

// Whether a case's break is outside every if/else and loop the case holds, in the module's order: no merge opened
// past the case's first label is still to come.
static bool at_case_end(const XscModule *m, uint32_t root, uint32_t exit)
{
	const size_t from = index_of(m, root);
	const size_t to = terminator_at(m, index_of(m, exit));
	uint32_t open[MAX_OPEN];
	size_t n = 0;
	for (size_t i = from; i < to && i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		while (ins->opcode == SPV_LABEL && n && open[n - 1] == ins->result_id)
			n--;
		if (ins->opcode != SPV_SELECTION_MERGE && ins->opcode != SPV_LOOP_MERGE)
			continue;
		if (n == MAX_OPEN)
			return false;
		open[n++] = ins->words[0];
	}
	return from <= to && n == 0;
}

static void visit(bool *seen, uint32_t *stack, size_t *top, uint32_t label)
{
	if (seen[label])
		return;
	seen[label] = true;
	stack[(*top)++] = label;
}

// The block of a case that breaks: the one branching to the merge, outside any if/else or loop of the case's own. A
// case that falls through to another, or leaves the switch other than by one such break, is refused.
static bool find_exit(Pass *p, Arm *arm)
{
	const XscModule *const m = p->m;
	bool *const seen = xsdk_xsc_alloc(p->x, m->id_limit);
	uint32_t *const stack = xsdk_xsc_alloc(p->x, (size_t)m->id_limit * sizeof *stack);
	if (!seen || !stack)
		return false;
	size_t top = 0;
	visit(seen, stack, &top, arm->root);
	while (top) {
		const uint32_t label = stack[--top];
		const size_t at = index_of(m, label);
		if (at <= p->header || at >= p->label)
			return refuse(p, "a switch's case leaving it other than by a break at its end (v1)");
		const XscInstr *const t = &m->instrs[terminator_at(m, at)];
		uint32_t next[MAX_LITERALS + 1];
		const uint32_t n = targets(t, next);
		for (uint32_t k = 0; k < n; k++) {
			const uint32_t to = next[k];
			if (to >= m->id_limit)
				return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a branch to %%%u, not a block", (unsigned)to);
			if (to == p->merge && (t->opcode != SPV_BRANCH || arm->exit))
				return refuse(p, "a switch's case leaving it other than by a break at its end (v1)");
			if (to != p->merge && to != arm->root && is_root(p, to))
				return refuse(p, "a switch's case falling through to another (v1)");
			if (to == p->merge)
				arm->exit = label;
			else
				visit(seen, stack, &top, to);
		}
	}
	if (arm->exit && !at_case_end(m, arm->root, arm->exit))
		return refuse(p, "a switch's case leaving it other than by a break at its end (v1)");
	return true;
}

static uint32_t constant_of(const XscModule *m, uint32_t type, uint32_t literal)
{
	for (size_t i = 0; i < m->instr_count && m->instrs[i].opcode != SPV_FUNCTION; i++) {
		const XscInstr *const c = &m->instrs[i];
		if (c->opcode == SPV_CONSTANT && c->result_type == type && c->word_count == 3 && c->words[2] == literal)
			return c->result_id;
	}
	return 0;
}

// The cases by first block, in order, those that are the default's left to it, and each literal's constant; then
// the default; each case's test and meeting; and each one's break.
static bool find_arms(Pass *p, const XscInstr *sw)
{
	const uint32_t def = sw->words[1];
	for (uint32_t k = 2; k + 1 < sw->word_count; k += 2) {
		if (sw->words[k + 1] == def)
			continue;
		size_t a = 0;
		while (a < p->arm_count && p->arms[a].root != sw->words[k + 1])
			a++;
		if (a == MAX_ARMS || p->literal_count == MAX_LITERALS)
			return refuse(p, "a switch of more cases than xost takes (v1)");
		p->arms[a].root = sw->words[k + 1];
		p->arm_count += a == p->arm_count;
		const uint32_t id = constant_of(p->m, p->selector_type, sw->words[k]);
		p->literals[p->literal_count++] = (Literal){ sw->words[k], id ? id : p->next_id++, !id };
	}
	for (size_t a = 0; a < p->arm_count; a++) {
		p->arms[a].test = a ? p->next_id++ : p->block;
		p->arms[a].meet = a ? p->next_id++ : p->merge;
	}
	const Arm *const last = p->arm_count ? &p->arms[p->arm_count - 1] : NULL;
	p->arms[p->arm_count] = (Arm){ def, 0, last ? last->test : p->block, last ? last->meet : p->merge };
	for (size_t a = 0; a <= p->arm_count; a++) {
		if (p->arms[a].root != p->merge && !find_exit(p, &p->arms[a]))
			return false;
	}
	return true;
}

// The selector a 32-bit int, the switch's block and merge, the bool type, and the cases.
static bool read_switch(Pass *p)
{
	const XscModule *const m = p->m;
	const XscInstr *const sw = &m->instrs[p->header + 1];
	const XscInstr *const selector = xsdk_xsc_by_result(m, sw->words[0]);
	const XscInstr *const type = selector ? xsdk_xsc_by_result(m, selector->result_type) : NULL;
	if (!type || type->opcode != SPV_TYPE_INT || type->words[1] != 32)
		return refuse(p, "a switch on other than a 32-bit int (v1)");
	p->selector_type = type->result_id;
	p->label = index_of(m, p->merge);
	for (size_t i = p->header; i > 0 && !p->block; i--)
		p->block = m->instrs[i].opcode == SPV_LABEL ? m->instrs[i].result_id : 0;
	if (p->label == m->instr_count || !p->block)
		return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a switch without its block or merge");
	for (size_t i = 0; i < m->instr_count && !p->bool_type; i++)
		p->bool_type = m->instrs[i].opcode == SPV_TYPE_BOOL ? m->instrs[i].result_id : 0;
	p->mint_bool = !p->bool_type;
	p->bool_type = p->bool_type ? p->bool_type : p->next_id++;
	while (p->label + 1 + p->phi_count < m->instr_count && m->instrs[p->label + 1 + p->phi_count].opcode == SPV_PHI)
		p->phi_count++;
	return find_arms(p, sw);
}

// The bool type and constants the chain needs that the module lacks, before its function.
static bool emit_minted(Pass *p)
{
	if (p->mint_bool && !emit_new(p, SPV_TYPE_BOOL, 0, p->bool_type, &p->bool_type, 1))
		return false;
	for (size_t i = 0; i < p->literal_count; i++) {
		const Literal *const l = &p->literals[i];
		const uint32_t words[3] = { p->selector_type, l->id, l->literal };
		if (l->minted && !emit_new(p, SPV_CONSTANT, p->selector_type, l->id, words, 3))
			return false;
	}
	return true;
}

// Case a's test, in its block: the selector equal to any of its literals.
static bool emit_test(Pass *p, size_t a, uint32_t *condition)
{
	const XscInstr *const sw = &p->m->instrs[p->header + 1];
	*condition = 0;
	for (uint32_t k = 2, n = 0; k + 1 < sw->word_count; k += 2) {
		if (sw->words[k + 1] == sw->words[1])
			continue;
		const uint32_t constant = p->literals[n++].id;
		if (sw->words[k + 1] != p->arms[a].root)
			continue;
		const uint32_t equal = p->next_id++;
		const uint32_t words[4] = { p->bool_type, equal, sw->words[0], constant };
		const uint32_t either = *condition ? p->next_id++ : equal;
		const uint32_t or_words[4] = { p->bool_type, either, *condition, equal };
		if (!emit_new(p, OP_I_EQUAL, p->bool_type, equal, words, 4) ||
			(*condition && !emit_new(p, OP_LOGICAL_OR, p->bool_type, either, or_words, 4)))
			return false;
		*condition = either;
	}
	return true;
}

// Case a's if/else: its test, where it meets the next, its case where the selector is one of its literals, else the
// next case's test, or after the last the default.
static bool emit_if(Pass *p, size_t a)
{
	const Arm *const arm = &p->arms[a];
	const Arm *const next = &p->arms[a + 1];
	uint32_t condition;
	if ((a && !emit_new(p, SPV_LABEL, 0, arm->test, &arm->test, 1)) || !emit_test(p, a, &condition))
		return false;
	const uint32_t merge[2] = { arm->meet, 0 };
	const uint32_t when = arm->root == p->merge ? arm->meet : arm->root;
	const uint32_t otherwise = a + 1 < p->arm_count ? next->test : next->root == p->merge ? arm->meet : next->root;
	const uint32_t branch[3] = { condition, when, otherwise };
	return emit_new(p, SPV_SELECTION_MERGE, 0, 0, merge, 2) && emit_new(p, SPV_BRANCH_CONDITIONAL, 0, 0, branch, 3);
}

static uint32_t phi_id(const Pass *p, size_t a, size_t f)
{
	return a ? p->phi_ids[a * p->phi_count + f] : p->m->instrs[p->label + 1 + f].result_id;
}

// An edge from case a into where its if/else meets, and its value for the merge's Phi phi: from the case's break, or
// from its test where it only breaks, as the edge from the switch's block was.
static bool edge(Pass *p, size_t a, const XscInstr *phi, uint32_t *words, uint32_t *n)
{
	const Arm *const arm = &p->arms[a];
	const uint32_t from = arm->root == p->merge ? arm->test : arm->exit;
	if (!from)
		return true;
	words[(*n)++] = incoming(phi, arm->root == p->merge ? p->block : arm->exit);
	words[(*n)++] = from;
	return words[*n - 2] || xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a Phi without a value from %%%u", (unsigned)from);
}

// Where case a's if/else meets: each of the merge's Phis, from the case, and from where the next one meets or after
// the last from the default.
static bool emit_phis(Pass *p, size_t a)
{
	for (size_t f = 0; f < p->phi_count; f++) {
		const XscInstr *const phi = &p->m->instrs[p->label + 1 + f];
		uint32_t words[6] = { phi->result_type, phi_id(p, a, f) };
		uint32_t n = 2;
		if (!edge(p, a, phi, words, &n))
			return false;
		if (a + 1 == p->arm_count && !edge(p, a + 1, phi, words, &n))
			return false;
		if (a + 1 < p->arm_count) {
			words[n++] = phi_id(p, a + 1, f);
			words[n++] = p->arms[a + 1].meet;
		}
		if (n == 2)
			return refuse(p, "a switch whose last case and default both leave it other than by a break (v1)");
		if (!emit_new(p, SPV_PHI, phi->result_type, words[1], words, n))
			return false;
	}
	return true;
}

// The blocks where each if/else after the first meets, then the merge's label and its Phis, now the first's.
static bool emit_meetings(Pass *p)
{
	for (size_t a = p->arm_count; a-- > 1;) {
		const uint32_t meet = p->arms[a].meet;
		if (!emit_new(p, SPV_LABEL, 0, meet, &meet, 1) || !emit_phis(p, a) ||
			!emit_new(p, SPV_BRANCH, 0, 0, &p->arms[a - 1].meet, 1))
			return false;
	}
	if (!emit(p, p->m->instrs[p->label]))
		return false;
	for (size_t f = 0; !p->arm_count && f < p->phi_count; f++) {
		if (!emit(p, p->m->instrs[p->label + 1 + f]))
			return false;
	}
	return !p->arm_count || emit_phis(p, 0);
}

// The case or default whose break is in block, or arm_count + 1.
static size_t arm_leaving(const Pass *p, uint32_t block)
{
	size_t a = 0;
	while (a <= p->arm_count && (p->arms[a].root == p->merge || p->arms[a].exit != block))
		a++;
	return a;
}

// A Phi of a case's first block from the switch's block, now from the case's test.
static bool emit_root_phi(Pass *p, const XscInstr *phi, uint32_t block)
{
	size_t a = 0;
	while (a <= p->arm_count && p->arms[a].root != block)
		a++;
	if (a > p->arm_count)
		return emit(p, *phi);
	uint32_t *const words = new_words(p, phi->words, phi->word_count);
	for (uint32_t k = 3; words && k < phi->word_count; k += 2)
		words[k] = words[k] == p->block ? p->arms[a].test : words[k];
	return words && emit(p, (XscInstr){ phi->opcode, phi->result_type, phi->result_id, words, phi->word_count });
}

// The module with the switch an if/else chain: the first test where the switch was and the others after it, where
// each meets before the merge, each case's break to where its if/else meets.
static bool emit_chain(Pass *p)
{
	const XscModule *const m = p->m;
	uint32_t block = 0;
	bool minted = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		block = ins->opcode == SPV_LABEL ? ins->result_id : block;
		const bool breaks = ins->opcode == SPV_BRANCH && ins->words[0] == p->merge;
		const size_t leaving = breaks ? arm_leaving(p, block) : p->arm_count + 1;
		bool ok = true;
		if (ins->opcode == SPV_FUNCTION && !minted) {
			minted = true;
			ok = emit_minted(p) && emit(p, *ins);
		} else if (i == p->header && !p->arm_count) {
			ok = emit_new(p, SPV_BRANCH, 0, 0, &m->instrs[i + 1].words[1], 1);
		} else if (i == p->header) {
			for (size_t a = 0; ok && a < p->arm_count; a++)
				ok = emit_if(p, a);
		} else if (i == p->header + 1 || (i > p->label && i <= p->label + p->phi_count)) {
			continue;
		} else if (i == p->label) {
			ok = emit_meetings(p);
		} else if (leaving <= p->arm_count) {
			ok = emit_new(p, SPV_BRANCH, 0, 0, &p->arms[leaving].meet, 1);
		} else if (ins->opcode == SPV_PHI) {
			ok = emit_root_phi(p, ins, block);
		} else {
			ok = emit(p, *ins);
		}
		if (!ok)
			return false;
	}
	return true;
}

static bool chain(Pass *p)
{
	if (!read_switch(p))
		return false;
	p->phi_ids = xsdk_xsc_alloc(p->x, (p->arm_count + 1) * (p->phi_count + 1) * sizeof *p->phi_ids);
	if (!p->phi_ids)
		return false;
	for (size_t i = p->phi_count; i < p->arm_count * p->phi_count; i++)
		p->phi_ids[i] = p->next_id++;
	return emit_chain(p);
}

// A switch's OpSelectionMerge, the OpSwitch after it.
static bool is_switch(const XscModule *m, size_t i)
{
	return i + 1 < m->instr_count && m->instrs[i].opcode == SPV_SELECTION_MERGE &&
		m->instrs[i + 1].opcode == SPV_SWITCH;
}

static bool rewrite(Xsc *x, XscModule *m, size_t header)
{
	Pass p = { .x = x,
		.m = m,
		.next_id = m->bound > m->id_limit ? m->bound : m->id_limit,
		.header = header,
		.merge = m->instrs[header].words[0] };
	const bool ok = is_wrapper(m, header) ? find_tail(&p) && unwrap(&p) : chain(&p);
	if (!ok)
		return false;
	m->instrs = p.out;
	m->instr_count = p.count;
	m->bound = p.next_id;
	return xsdk_xsc_spirv_index(x, m);
}

bool xsdk_xsc_switches(Xsc *x, XscModule *m)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		if (is_switch(m, i) && !rewrite(x, m, i))
			return false;
	}
	return true;
}
