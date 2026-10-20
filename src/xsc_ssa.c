#include <string.h>

#include "xsc_private.h"

// Loops in SSA form, as glslang's HLSL leaves them after spirv-opt's legalisation, put in the memory form glslang
// writes for GLSL, the shape the unroller takes (xsc_unroll.c, expand_loop): each Phi a Function variable, stored on
// each edge into its block and loaded where it was; each loop's header its merge and a branch alone, what it held in a
// test block after it; and a continue block of its own after the block branching back, holding a counter's step as
// GLSL's for does.

enum {
	OP_FUNCTION_CALL = 57,
	OP_UNREACHABLE = 255,
	STORAGE_UNIFORM_CONSTANT = 0,
	STORAGE_INPUT = 1,
	STORAGE_UNIFORM = 2,
	STORAGE_PUSH_CONSTANT = 9,
};

typedef struct {
	XscInstr label;
	XscInstr *code;
	size_t count;
	size_t capacity;
} Block;

// A loop in the unroller's shape: its header, merge, test block, the block branching back and its continue block;
// whether it tests at the top, in the test block, which the unroller doesn't walk where it unrolls or counts the loop
// (xsc_unroll.c, expand_loop), or at the end, a do/while; and the blocks it holds.
typedef struct {
	uint32_t header;
	uint32_t merge;
	uint32_t test;
	uint32_t latch;
	uint32_t cont;
	bool at_top;
	bool at_end;
	bool *member;
} Loop;

// A Phi as it was, its block, and whether it holds its value in its loop header's Phi's variable.
typedef struct {
	size_t block;
	XscInstr ins;
	bool shares;
} Phi;

// A value read again in a block, and its id there.
typedef struct {
	size_t block;
	uint32_t from;
	uint32_t to;
} Copy;

typedef struct {
	Xsc *x;
	XscModule *m;
	size_t function;
	uint32_t limit;
	uint32_t next_id;
	Block *blocks;
	size_t block_count;
	size_t block_capacity;
	Loop *loops;
	size_t loop_count;
	size_t loop_capacity;
	Phi *phis;
	size_t phi_count;
	size_t phi_capacity;
	XscInstr *types;
	size_t type_count;
	size_t type_capacity;
	XscInstr *vars;
	size_t var_count;
	size_t var_capacity;
	Copy *copies;
	size_t copy_count;
	size_t copy_capacity;
	XscInstr *prefix;
	size_t prefix_count;
	size_t prefix_capacity;
	// By id below limit: a Phi's variable, the block defining a value (-1 for none), its uses, the continue block a
	// counter's step moved to, and whether a header's Phi's variable holds a merge's Phi too.
	uint32_t *phi_var;
	int32_t *def;
	uint32_t *uses;
	uint32_t *moved_to;
	bool *shared;
} Pass;

static bool refuse(Pass *p, const char *message)
{
	return xsdk_xsc_fail(p->x, XSDK_XSC_REFUSED, "%s", message);
}

static uint32_t fresh(Pass *p)
{
	return p->next_id++;
}

static bool add(Pass *p, XscInstr **list, size_t *count, size_t *capacity, XscInstr ins)
{
	if (!xsdk_xsc_room(p->x, list, *count, capacity, sizeof **list))
		return false;
	(*list)[(*count)++] = ins;
	return true;
}

static uint32_t *copy_words(Pass *p, const uint32_t *words, uint32_t count)
{
	uint32_t *const copy = xsdk_xsc_alloc(p->x, (count ? count : 1) * sizeof *copy);
	if (copy)
		memcpy(copy, words, count * sizeof *copy);
	return copy;
}

static bool make(Pass *p, XscInstr *out, uint32_t opcode, uint32_t type, uint32_t id, const uint32_t *words, uint32_t n)
{
	const uint32_t *const copy = copy_words(p, words, n);
	*out = (XscInstr){ opcode, type, id, copy, n };
	return copy != NULL;
}

static bool branch_to(Pass *p, XscInstr *out, uint32_t label)
{
	return make(p, out, SPV_BRANCH, 0, 0, &label, 1);
}

static uint32_t label_of(const Block *b)
{
	return b->label.result_id;
}

static size_t block_of(const Pass *p, uint32_t label)
{
	for (size_t i = 0; i < p->block_count; i++) {
		if (label_of(&p->blocks[i]) == label)
			return i;
	}
	return p->block_count;
}

static XscInstr *terminator(const Block *b)
{
	return &b->code[b->count - 1];
}

static size_t successors(const Block *b, uint32_t next[2])
{
	const XscInstr *const t = terminator(b);
	if (t->opcode == SPV_BRANCH) {
		next[0] = t->words[0];
		return 1;
	}
	if (t->opcode != SPV_BRANCH_CONDITIONAL)
		return 0;
	next[0] = t->words[1];
	next[1] = t->words[2];
	return 2;
}

static bool branches_to(const Block *b, uint32_t label)
{
	uint32_t next[2];
	const size_t n = successors(b, next);
	return (n > 0 && next[0] == label) || (n > 1 && next[1] == label);
}

static bool insert_at(Pass *p, Block *b, size_t at, XscInstr ins)
{
	if (!xsdk_xsc_room(p->x, &b->code, b->count, &b->capacity, sizeof *b->code))
		return false;
	memmove(&b->code[at + 1], &b->code[at], (b->count - at) * sizeof *b->code);
	b->code[at] = ins;
	b->count++;
	return true;
}

static bool insert_block(Pass *p, size_t at, Block b)
{
	if (!xsdk_xsc_room(p->x, &p->blocks, p->block_count, &p->block_capacity, sizeof *p->blocks))
		return false;
	memmove(&p->blocks[at + 1], &p->blocks[at], (p->block_count - at) * sizeof *p->blocks);
	p->blocks[at] = b;
	p->block_count++;
	return true;
}

// A block of one terminator, label, at at.
static bool new_block(Pass *p, size_t at, uint32_t label, XscInstr last)
{
	Block b = { 0 };
	return make(p, &b.label, SPV_LABEL, 0, label, &label, 1) && add(p, &b.code, &b.count, &b.capacity, last) &&
		insert_block(p, at, b);
}

static bool ends_block(uint32_t opcode)
{
	return opcode >= SPV_BRANCH && opcode <= OP_UNREACHABLE;
}

// The function's blocks, each its label, then all to its terminator.
static bool find_blocks(Pass *p, size_t first, size_t end)
{
	Block *b = NULL;
	for (size_t i = first; i < end; i++) {
		const XscInstr *const ins = &p->m->instrs[i];
		if (ins->opcode == SPV_LABEL) {
			if (!xsdk_xsc_room(p->x, &p->blocks, p->block_count, &p->block_capacity, sizeof *p->blocks))
				return false;
			b = &p->blocks[p->block_count++];
			*b = (Block){ .label = *ins };
		} else if (!b || (b->count && ends_block(terminator(b)->opcode))) {
			return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "an instruction outside a block");
		} else if (!add(p, &b->code, &b->count, &b->capacity, *ins)) {
			return false;
		}
	}
	for (size_t i = 0; i < p->block_count; i++) {
		if (!p->blocks[i].count || !ends_block(terminator(&p->blocks[i])->opcode))
			return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a block without a terminator");
	}
	return true;
}

// A header's OpLoopMerge, just before its branch (SPIR-V 1.0, section 3.32.17, OpLoopMerge), or NULL.
static const XscInstr *loop_merge(const Block *b)
{
	return b->count >= 2 && b->code[b->count - 2].opcode == SPV_LOOP_MERGE ? &b->code[b->count - 2] : NULL;
}

// A loop's header holding more than its merge and a branch.
static bool in_ssa_form(const Block *b)
{
	return loop_merge(b) && (b->count != 2 || b->code[1].opcode != SPV_BRANCH);
}

// Whether word k of an instruction is a value's id, not its result type or id, a label or a literal.
static bool value_word(const XscInstr *ins, uint32_t k)
{
	switch (ins->opcode) {
	case SPV_PHI:
		return k >= 2 && k % 2 == 0;
	case SPV_BRANCH_CONDITIONAL:
	case SPV_RETURN_VALUE:
		return k == 0;
	case SPV_STORE:
		return k < 2;
	case SPV_LOAD:
		return k == 2;
	case SPV_LABEL:
	case SPV_LOOP_MERGE:
	case SPV_SELECTION_MERGE:
	case SPV_BRANCH:
	case SPV_VARIABLE:
		return false;
	default: {
		uint32_t count;
		const uint32_t skip = (uint32_t)(xsdk_xsc_operands(ins, &count) - ins->words);
		return k >= skip && xsdk_xsc_id_operand(ins->opcode, k - skip);
	}
	}
}

// The blocks reachable from a loop's header without passing its merge: the loop's (SPIR-V 1.0, section 2.11).
static bool find_members(Pass *p, uint32_t header, uint32_t merge, bool **member)
{
	*member = xsdk_xsc_alloc(p->x, p->block_count);
	size_t *const stack = xsdk_xsc_alloc(p->x, p->block_count * sizeof *stack);
	const size_t h = block_of(p, header);
	if (!*member || !stack)
		return false;
	size_t top = 0;
	(*member)[h] = true;
	stack[top++] = h;
	while (top) {
		uint32_t next[2];
		const size_t n = successors(&p->blocks[stack[--top]], next);
		for (size_t k = 0; k < n; k++) {
			const size_t s = block_of(p, next[k]);
			if (s == p->block_count || next[k] == merge || (*member)[s])
				continue;
			(*member)[s] = true;
			stack[top++] = s;
		}
	}
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

static bool int_constant(const Pass *p, uint32_t id)
{
	const XscInstr *const c = xsdk_xsc_by_result(p->m, id);
	const XscInstr *const type = c ? xsdk_xsc_by_result(p->m, c->result_type) : NULL;
	return c && c->opcode == SPV_CONSTANT && type && type->opcode == SPV_TYPE_INT;
}

// i + k, k + i or i - k, k an int constant: a counter's step.
static bool is_step(const Pass *p, const XscInstr *ins, uint32_t i)
{
	if ((ins->opcode != SPV_I_ADD && ins->opcode != SPV_I_SUB) || ins->word_count != 4)
		return false;
	const uint32_t a = ins->words[2];
	const uint32_t b = ins->words[3];
	return (a == i && int_constant(p, b)) || (ins->opcode == SPV_I_ADD && b == i && int_constant(p, a));
}

static size_t find_def(const Block *b, uint32_t id)
{
	size_t i = 0;
	while (i < b->count && b->code[i].result_id != id)
		i++;
	return i;
}

// Each counter's step the block branching back makes for its header's Phi alone, moved to the continue block, as
// glslang writes GLSL's for (xsc_unroll.c, block_step).
static bool move_steps(Pass *p, const Loop *l)
{
	Block *const test = &p->blocks[block_of(p, l->test)];
	Block *const back = &p->blocks[block_of(p, l->latch)];
	Block *const cont = &p->blocks[block_of(p, l->cont)];
	for (size_t i = 0; i < test->count && test->code[i].opcode == SPV_PHI; i++) {
		const uint32_t step = incoming(&test->code[i], l->latch);
		const size_t at = find_def(back, step);
		if (!step || step >= p->limit || p->uses[step] != 1 || at == back->count ||
			!is_step(p, &back->code[at], test->code[i].result_id))
			continue;
		const XscInstr ins = back->code[at];
		memmove(&back->code[at], &back->code[at + 1], (back->count - at - 1) * sizeof *back->code);
		back->count--;
		if (!insert_at(p, cont, cont->count - 1, ins))
			return false;
		p->moved_to[step] = l->cont;
	}
	return true;
}

// The block branching back to the header, which must be the loop's one, ending in a branch to the header or a
// do/while's test; its terminator goes to a continue block after it, which takes the loop merge's continue.
static bool add_continue(Pass *p, Loop *l, const bool *member)
{
	size_t latch = p->block_count, latches = 0;
	for (size_t b = 0; b < p->block_count; b++) {
		if (member[b] && branches_to(&p->blocks[b], l->header)) {
			latch = b;
			latches++;
		}
	}
	if (latches != 1)
		return refuse(p, "a loop with other than one branch back to its header (v1)");
	Block *const back = &p->blocks[latch];
	const XscInstr last = *terminator(back);
	l->latch = label_of(back);
	l->at_end = last.opcode == SPV_BRANCH_CONDITIONAL;
	const bool merged = back->count >= 2 && back->code[back->count - 2].opcode == SPV_SELECTION_MERGE;
	if (l->at_end && (merged || !branches_to(back, l->merge)))
		return refuse(p, "a loop branching back on a condition other than a do/while's test (v1)");
	return branch_to(p, terminator(back), l->cont) && new_block(p, latch + 1, l->cont, last);
}

// The header its merge and a branch alone, all else in a test block after it, and a continue block.
static bool reshape(Pass *p, size_t h)
{
	Block test = p->blocks[h];
	const XscInstr merge = test.code[test.count - 2];
	Loop l = { .header = label_of(&test), .merge = merge.words[0] };
	l.test = fresh(p);
	l.cont = fresh(p);
	uint32_t *const words = copy_words(p, merge.words, merge.word_count);
	Block head = { .label = test.label };
	XscInstr branch;
	if (!words || !branch_to(p, &branch, l.test) || !make(p, &test.label, SPV_LABEL, 0, l.test, &l.test, 1))
		return false;
	words[1] = l.cont;
	test.code[test.count - 2] = test.code[test.count - 1];
	test.count--;
	if (!add(p, &head.code, &head.count, &head.capacity, (XscInstr){ SPV_LOOP_MERGE, 0, 0, words, merge.word_count }) ||
		!add(p, &head.code, &head.count, &head.capacity, branch))
		return false;
	p->blocks[h] = head;
	bool *member;
	if (!insert_block(p, h + 1, test) || !find_members(p, l.header, l.merge, &member) || !add_continue(p, &l, member) ||
		(!l.at_end && !move_steps(p, &l)))
		return false;
	// A do/while of one block tested there; it's the continue block's now.
	l.at_top = terminator(&p->blocks[block_of(p, l.test)])->opcode == SPV_BRANCH_CONDITIONAL;
	if (!xsdk_xsc_room(p->x, &p->loops, p->loop_count, &p->loop_capacity, sizeof *p->loops))
		return false;
	p->loops[p->loop_count++] = l;
	return true;
}

// The id of a pointer to type in Function storage: the module's, or one made.
static bool function_pointer(Pass *p, uint32_t type, uint32_t *out)
{
	for (size_t i = 0; i < p->function + p->type_count; i++) {
		const XscInstr *const ins = i < p->function ? &p->m->instrs[i] : &p->types[i - p->function];
		if (ins->opcode == SPV_TYPE_POINTER && ins->words[1] == SPV_STORAGE_FUNCTION && ins->words[2] == type) {
			*out = ins->result_id;
			return true;
		}
	}
	XscInstr pointer;
	*out = fresh(p);
	return make(p, &pointer, SPV_TYPE_POINTER, 0, *out, (const uint32_t[]){ *out, SPV_STORAGE_FUNCTION, type }, 3) &&
		add(p, &p->types, &p->type_count, &p->type_capacity, pointer);
}

static bool new_var(Pass *p, uint32_t type, uint32_t *out)
{
	uint32_t pointer;
	XscInstr var;
	if (!function_pointer(p, type, &pointer))
		return false;
	*out = fresh(p);
	return make(p, &var, SPV_VARIABLE, pointer, *out, (const uint32_t[]){ pointer, *out, SPV_STORAGE_FUNCTION }, 3) &&
		add(p, &p->vars, &p->var_count, &p->var_capacity, var);
}

// A header's label stands for its test block, which took what it held.
static uint32_t remapped(const Pass *p, uint32_t label)
{
	for (size_t i = 0; i < p->loop_count; i++) {
		if (p->loops[i].header == label)
			return p->loops[i].test;
	}
	return label;
}

static const Loop *loop_tested_in(const Pass *p, uint32_t label)
{
	for (size_t i = 0; i < p->loop_count; i++) {
		if (p->loops[i].test == label)
			return &p->loops[i];
	}
	return NULL;
}

// A store at the end of a block, before its merge and branch, or the continue block's where the value is a step moved
// there.
static bool store_at_end(Pass *p, uint32_t label, uint32_t var, uint32_t value)
{
	if (value < p->limit && p->moved_to[value])
		label = p->moved_to[value];
	const size_t b = block_of(p, label);
	if (b == p->block_count)
		return xsdk_xsc_fail(p->x, XSDK_XSC_MALFORMED, "a Phi from %%%u, not a block", (unsigned)label);
	Block *const blk = &p->blocks[b];
	size_t at = blk->count - 1;
	const uint32_t before = at ? blk->code[at - 1].opcode : 0;
	at -= before == SPV_SELECTION_MERGE || before == SPV_LOOP_MERGE;
	XscInstr store;
	return make(p, &store, SPV_STORE, 0, 0, (const uint32_t[]){ var, value }, 2) && insert_at(p, blk, at, store);
}

static const Phi *phi_of(const Pass *p, uint32_t id, size_t block)
{
	for (size_t i = 0; i < p->phi_count; i++) {
		if (p->phis[i].ins.result_id == id && p->phis[i].block == block)
			return &p->phis[i];
	}
	return NULL;
}

// What a loop leaves with from its test, made before the loop, stored on each edge into its header; one of the header's
// Phis' values is held in its variable (sharing).
static bool store_on_entry(Pass *p, const Loop *l, uint32_t var, uint32_t value)
{
	const size_t test = block_of(p, l->test);
	if (phi_of(p, value, test))
		return refuse(p, "a loop's header's Phi left with apart from its own variable (v1)");
	if (find_def(&p->blocks[test], value) < p->blocks[test].count)
		return refuse(p, "a value a loop's test works out, kept past the loop (v1)");
	for (size_t b = 0; b < p->block_count; b++) {
		const uint32_t label = label_of(&p->blocks[b]);
		if (label != l->cont && branches_to(&p->blocks[b], l->header) && !store_at_end(p, label, var, value))
			return false;
	}
	return store_at_end(p, l->latch, var, value);
}

// A Phi's value stored on the edge from block from; none from a loop's test where its header's Phi's variable holds
// it already.
static bool place(Pass *p, const Phi *phi, uint32_t value, uint32_t from)
{
	const uint32_t var = p->phi_var[phi->ins.result_id];
	from = remapped(p, from);
	const Loop *const l = loop_tested_in(p, from);
	if (!l || !l->at_top)
		return store_at_end(p, from, var, value);
	if (label_of(&p->blocks[phi->block]) != l->merge)
		return refuse(p, "a Phi on the edge from a loop's test into its pass (v1)");
	return phi->shares || store_on_entry(p, l, var, value);
}

// Whether an instruction after a loop reads id there: a Phi reads its value for an edge where it's stored, at the end
// of the block the edge leaves.
static bool reads_after(const Pass *p, const Loop *l, const XscInstr *ins, uint32_t id)
{
	for (uint32_t k = 0; k < ins->word_count; k++) {
		if (ins->words[k] != id || !value_word(ins, k))
			continue;
		const size_t from = ins->opcode == SPV_PHI ? block_of(p, remapped(p, ins->words[k + 1])) : p->block_count;
		if (from == p->block_count || !l->member[from])
			return true;
	}
	return false;
}

// A loop's merge's Phi of its header's Phi from the test and a value from each block that only breaks, which GLSL's
// form holds in one local: the header's Phi's variable, stored at each break, where nothing after the loop reads that
// Phi but this one.
static const Phi *sharing(const Pass *p, const Phi *phi)
{
	const Loop *l = NULL;
	for (size_t i = 0; i < p->loop_count && !l; i++)
		l = p->loops[i].merge == label_of(&p->blocks[phi->block]) && p->loops[i].at_top ? &p->loops[i] : NULL;
	const Phi *header = NULL;
	for (uint32_t k = 2; l && k + 1 < phi->ins.word_count; k += 2) {
		const uint32_t from = remapped(p, phi->ins.words[k + 1]);
		const size_t b = block_of(p, from);
		uint32_t next[2];
		if (from == l->test)
			header = phi_of(p, phi->ins.words[k], block_of(p, l->test));
		else if (b == p->block_count || successors(&p->blocks[b], next) != 1)
			return NULL;
	}
	if (!header || p->shared[header->ins.result_id])
		return NULL;
	for (size_t b = 0; b < p->block_count; b++) {
		for (size_t i = 0; !l->member[b] && i < p->blocks[b].count; i++) {
			const XscInstr *const ins = &p->blocks[b].code[i];
			if (ins->result_id != phi->ins.result_id && reads_after(p, l, ins, header->ins.result_id))
				return NULL;
		}
	}
	return header;
}

// Each Phi a Function variable, stored on each edge into its block and loaded where it was (SPIR-V 1.0, section
// 3.32.17, OpPhi).
static bool convert_phis(Pass *p)
{
	for (size_t b = 0; b < p->block_count; b++) {
		for (size_t i = 0; i < p->blocks[b].count; i++) {
			if (p->blocks[b].code[i].opcode != SPV_PHI)
				continue;
			if (!xsdk_xsc_room(p->x, &p->phis, p->phi_count, &p->phi_capacity, sizeof *p->phis))
				return false;
			p->phis[p->phi_count++] = (Phi){ .block = b, .ins = p->blocks[b].code[i] };
		}
	}
	const Phi **const header = xsdk_xsc_alloc(p->x, (p->phi_count + 1) * sizeof *header);
	if (!header)
		return false;
	for (size_t i = 0; i < p->phi_count; i++) {
		header[i] = sharing(p, &p->phis[i]);
		p->phis[i].shares = header[i] != NULL;
		if (header[i])
			p->shared[header[i]->ins.result_id] = true;
		else if (!new_var(p, p->phis[i].ins.result_type, &p->phi_var[p->phis[i].ins.result_id]))
			return false;
	}
	for (size_t i = 0; i < p->phi_count; i++) {
		if (header[i])
			p->phi_var[p->phis[i].ins.result_id] = p->phi_var[header[i]->ins.result_id];
	}
	for (size_t i = 0; i < p->phi_count; i++) {
		const XscInstr *const phi = &p->phis[i].ins;
		for (uint32_t k = 2; k + 1 < phi->word_count; k += 2) {
			if (!place(p, &p->phis[i], phi->words[k], phi->words[k + 1]))
				return false;
		}
	}
	for (size_t i = 0; i < p->phi_count; i++) {
		const XscInstr *const phi = &p->phis[i].ins;
		Block *const b = &p->blocks[p->phis[i].block];
		const uint32_t words[3] = { phi->result_type, phi->result_id, p->phi_var[phi->result_id] };
		if (!make(p, &b->code[find_def(b, phi->result_id)], SPV_LOAD, words[0], words[1], words, 3))
			return false;
	}
	return true;
}

static bool copied(const Pass *p, size_t b, uint32_t id, uint32_t *out)
{
	for (size_t i = 0; i < p->copy_count; i++) {
		if (p->copies[i].block == b && p->copies[i].from == id) {
			*out = p->copies[i].to;
			return true;
		}
	}
	return false;
}

// An instruction at the block's top standing for id there.
static bool put_copy(Pass *p, size_t b, uint32_t id, XscInstr ins)
{
	if (!add(p, &p->prefix, &p->prefix_count, &p->prefix_capacity, ins) ||
		!xsdk_xsc_room(p->x, &p->copies, p->copy_count, &p->copy_capacity, sizeof *p->copies))
		return false;
	p->copies[p->copy_count++] = (Copy){ b, id, ins.result_id };
	return true;
}

static bool reload(Pass *p, size_t b, uint32_t id, uint32_t type, uint32_t var, uint32_t *out)
{
	XscInstr load;
	*out = fresh(p);
	return make(p, &load, SPV_LOAD, type, *out, (const uint32_t[]){ type, *out, var }, 3) && put_copy(p, b, id, load);
}

// Whether a pointer reaches what the shader can't write: an input, a uniform or push constant, or a sampler.
static bool read_only(const Pass *p, uint32_t pointer)
{
	const XscInstr *v = xsdk_xsc_by_result(p->m, pointer);
	while (v && (v->opcode == SPV_ACCESS_CHAIN || v->opcode == SPV_IN_BOUNDS_ACCESS_CHAIN))
		v = xsdk_xsc_by_result(p->m, v->words[2]);
	const uint32_t storage = v && v->opcode == SPV_VARIABLE ? v->words[2] : SPV_STORAGE_FUNCTION;
	return storage == STORAGE_UNIFORM_CONSTANT || storage == STORAGE_INPUT || storage == STORAGE_UNIFORM ||
		storage == STORAGE_PUSH_CONSTANT;
}

static bool operand_in(Pass *p, size_t b, uint32_t id, uint32_t *out);

// A value a loop's test block works out, which the unroller doesn't walk where it unrolls or counts the loop, worked
// out again where it's read: of no store or call, and a load only of what the shader can't write.
static bool repeat(Pass *p, size_t b, const XscInstr *ins, uint32_t *out)
{
	uint32_t count;
	const uint32_t skip = (uint32_t)(xsdk_xsc_operands(ins, &count) - ins->words);
	const bool loads = ins->opcode == SPV_LOAD;
	if (skip != 2 || ins->opcode == OP_FUNCTION_CALL || (loads && !read_only(p, ins->words[2])))
		return refuse(p, "a value a loop's test works out and its pass reads, which xost can't work out again (v1)");
	uint32_t *const words = copy_words(p, ins->words, ins->word_count);
	if (!words)
		return false;
	for (uint32_t k = 2; k < ins->word_count; k++) {
		if (value_word(ins, k) && !operand_in(p, b, ins->words[k], &words[k]))
			return false;
	}
	*out = words[1] = fresh(p);
	return put_copy(p, b, ins->result_id, (XscInstr){ ins->opcode, ins->result_type, *out, words, ins->word_count });
}

// A Phi of the loop's header taking value on the branch back.
static const Phi *taking(const Pass *p, const Loop *l, uint32_t value)
{
	const size_t test = block_of(p, l->test);
	for (size_t i = 0; i < p->phi_count; i++) {
		const Phi *const phi = &p->phis[i];
		for (uint32_t k = 2; phi->block == test && k + 1 < phi->ins.word_count; k += 2) {
			if (phi->ins.words[k] == value && remapped(p, phi->ins.words[k + 1]) == l->latch)
				return phi;
		}
	}
	return NULL;
}

// A value a loop's pass works out, read after the loop: in a do/while, one its header's Phi takes on the branch back,
// which that Phi's variable holds after the loop.
static bool carried_out(Pass *p, size_t b, size_t d, const XscInstr *ins, uint32_t *out)
{
	for (size_t i = 0; i < p->loop_count; i++) {
		const Loop *const l = &p->loops[i];
		if (!l->member[d] || l->member[b])
			continue;
		const Phi *const phi = l->at_end ? taking(p, l, ins->result_id) : NULL;
		if (!phi)
			return refuse(
				p, "a value a loop's pass works out, read after the loop, other than one a do/while carries (v1)");
		return reload(p, b, ins->result_id, ins->result_type, p->phi_var[phi->ins.result_id], out);
	}
	return true;
}

// The id block b reads for a value made in another: the value itself where the unroller carries it there, or one
// loaded or worked out again at b's top.
static bool operand_in(Pass *p, size_t b, uint32_t id, uint32_t *out)
{
	*out = id;
	if (id >= p->limit || p->def[id] < 0 || (size_t)p->def[id] == b || copied(p, b, id, out))
		return true;
	const size_t d = (size_t)p->def[id];
	const XscInstr *const ins = &p->blocks[d].code[find_def(&p->blocks[d], id)];
	const Loop *const l = loop_tested_in(p, label_of(&p->blocks[d]));
	if (p->phi_var[id] && l && l->at_end && !l->member[b])
		return refuse(p, "a do/while's header's Phi read after the loop (v1)");
	if (p->phi_var[id])
		return reload(p, b, id, ins->result_type, p->phi_var[id], out);
	if (l && l->at_top)
		return repeat(p, b, ins, out);
	return carried_out(p, b, d, ins, out);
}

// Each value the block reads from another as the unroller takes it, what it loads or works out again at its top,
// after its Phis' loads.
static bool rewrite_block(Pass *p, size_t b)
{
	p->prefix_count = 0;
	for (size_t i = 0; i < p->blocks[b].count; i++) {
		XscInstr *const ins = &p->blocks[b].code[i];
		uint32_t *words = NULL;
		for (uint32_t k = 0; k < ins->word_count; k++) {
			uint32_t id;
			if (!value_word(ins, k) || !operand_in(p, b, ins->words[k], &id))
				continue;
			if (!words && id != ins->words[k] && !(words = copy_words(p, ins->words, ins->word_count)))
				return false;
			if (words)
				words[k] = id;
		}
		if (p->x->status != XSDK_XSC_OK)
			return false;
		ins->words = words ? words : ins->words;
	}
	Block *const blk = &p->blocks[b];
	size_t at = 0;
	while (at < blk->count && blk->code[at].opcode == SPV_LOAD && blk->code[at].result_id < p->limit &&
		p->phi_var[blk->code[at].result_id])
		at++;
	for (size_t i = 0; i < p->prefix_count; i++) {
		if (!insert_at(p, blk, at + i, p->prefix[i]))
			return false;
	}
	return true;
}

// The module with the new pointer types before the function, the variables in its first block (SPIR-V 1.0, section
// 2.4) and the blocks as they are now.
static bool emit(Pass *p, size_t first, size_t end)
{
	XscModule *const m = p->m;
	size_t total = m->instr_count - (end - first) + p->type_count + p->var_count;
	for (size_t b = 0; b < p->block_count; b++)
		total += 1 + p->blocks[b].count;
	XscInstr *const out = xsdk_xsc_alloc(p->x, total * sizeof *out);
	if (!out)
		return false;
	size_t n = 0;
	memcpy(out, m->instrs, p->function * sizeof *out);
	n += p->function;
	memcpy(out + n, p->types, p->type_count * sizeof *out);
	n += p->type_count;
	memcpy(out + n, m->instrs + p->function, (first - p->function) * sizeof *out);
	n += first - p->function;
	for (size_t b = 0; b < p->block_count; b++) {
		const Block *const blk = &p->blocks[b];
		size_t i = 0;
		out[n++] = blk->label;
		while (!b && i < blk->count && blk->code[i].opcode == SPV_VARIABLE)
			out[n++] = blk->code[i++];
		if (!b)
			memcpy(out + n, p->vars, p->var_count * sizeof *out);
		n += b ? 0 : p->var_count;
		memcpy(out + n, blk->code + i, (blk->count - i) * sizeof *out);
		n += blk->count - i;
	}
	memcpy(out + n, m->instrs + end, (m->instr_count - end) * sizeof *out);
	m->instrs = out;
	m->instr_count = n + m->instr_count - end;
	m->bound = p->next_id;
	return xsdk_xsc_spirv_index(p->x, m);
}

static bool count_uses(Pass *p, size_t first, size_t end)
{
	p->phi_var = xsdk_xsc_alloc(p->x, p->limit * sizeof *p->phi_var);
	p->def = xsdk_xsc_alloc(p->x, p->limit * sizeof *p->def);
	p->uses = xsdk_xsc_alloc(p->x, p->limit * sizeof *p->uses);
	p->moved_to = xsdk_xsc_alloc(p->x, p->limit * sizeof *p->moved_to);
	p->shared = xsdk_xsc_alloc(p->x, p->limit);
	if (!p->phi_var || !p->def || !p->uses || !p->moved_to || !p->shared)
		return false;
	for (size_t i = first; i < end; i++) {
		const XscInstr *const ins = &p->m->instrs[i];
		for (uint32_t k = 0; k < ins->word_count; k++) {
			if (ins->words[k] < p->limit && value_word(ins, k))
				p->uses[ins->words[k]]++;
		}
	}
	return true;
}

// Each value's block.
static void find_defs(Pass *p)
{
	memset(p->def, 0xFF, p->limit * sizeof *p->def);
	for (size_t b = 0; b < p->block_count; b++) {
		for (size_t i = 0; i < p->blocks[b].count; i++) {
			const uint32_t id = p->blocks[b].code[i].result_id;
			if (id && id < p->limit)
				p->def[id] = (int32_t)b;
		}
	}
}

// glslang writes a GLSL loop's header as its label, merge and branch alone; anything else came from spirv-opt.
static bool has_ssa_loop(const XscModule *m)
{
	for (size_t i = 1; i + 1 < m->instr_count; i++) {
		const bool merge = m->instrs[i].opcode == SPV_LOOP_MERGE;
		if (merge && (m->instrs[i - 1].opcode != SPV_LABEL || m->instrs[i + 1].opcode != SPV_BRANCH))
			return true;
	}
	return false;
}

static bool find_index(const XscModule *m, size_t from, uint32_t opcode, size_t *at)
{
	for (*at = from; *at < m->instr_count; (*at)++) {
		if (m->instrs[*at].opcode == opcode)
			return true;
	}
	return false;
}

bool xsdk_xsc_ssa_loops(Xsc *x, XscModule *m)
{
	Pass p = { .x = x, .m = m, .limit = m->id_limit, .next_id = m->bound > m->id_limit ? m->bound : m->id_limit };
	size_t first, end;
	if (!has_ssa_loop(m) || !find_index(m, 0, SPV_FUNCTION, &p.function) ||
		!find_index(m, p.function, SPV_LABEL, &first) || !find_index(m, p.function, SPV_FUNCTION_END, &end) ||
		first > end)
		return true;
	if (!find_blocks(&p, first, end))
		return false;
	for (size_t b = 0; b < p.block_count; b++) {
		if (terminator(&p.blocks[b])->opcode == SPV_SWITCH)
			return refuse(&p, "a switch (v1)");
	}
	if (!count_uses(&p, first, end))
		return false;
	for (size_t h = 0; h < p.block_count; h++) {
		if (in_ssa_form(&p.blocks[h]) && !reshape(&p, h))
			return false;
	}
	for (size_t i = 0; i < p.loop_count; i++) {
		if (!find_members(&p, p.loops[i].header, p.loops[i].merge, &p.loops[i].member))
			return false;
	}
	if (!convert_phis(&p))
		return false;
	find_defs(&p);
	for (size_t b = 0; b < p.block_count; b++) {
		if (!rewrite_block(&p, b))
			return false;
	}
	return emit(&p, first, end);
}
