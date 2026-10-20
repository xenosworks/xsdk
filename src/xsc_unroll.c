#include <math.h>
#include <stdio.h>
#include <string.h>

#include "xsc_private.h"

// Loops unrolled and locals turned into SSA values, as xenos-toolchain/xsc/unroll.py does it. An if/else on data runs
// both arms, and each local, output and Phi they leave apart is merged by a select where they meet. A loop a block's
// int counts, or whose count is data, is walked once, for a hardware loop, what it carries a value at the top of each
// pass; a break or continue on data in its pass is gated as in an unrolled loop, alive a value it carries from 1.

enum {
	MAX_TRIP = 4096,
	OP_I_ADD = 128,
	OP_I_SUB = 130,
	OP_I_MUL = 132,
	OP_S_DIV = 135,
	OP_I_EQUAL = 170,
	OP_S_GREATER_THAN = 173,
	OP_S_LESS_THAN_EQUAL = 179,
	OP_IN_BOUNDS_ACCESS_CHAIN = 66,
	SC_UNIFORM_CONSTANT = 0,
	SC_UNIFORM = 2,
	SC_OUTPUT = 3,
	SC_PUSH_CONSTANT = 9,
	OP_LOGICAL_AND = 167,
	OP_LOGICAL_NOT = 168,
	OP_UNREACHABLE = 255,
	BUILTIN_POSITION = 0,
	BUILTIN_FRAG_DEPTH = 22,
	MODEL_VERTEX = 0,
	// More outputs than a vertex shader exports (16 interpolators and the position) or a pixel shader.
	MAX_SLOTS = 32,
	MESSAGE_BYTES = 96,
};

typedef struct {
	uint32_t id;
	int64_t value;
} IntConst;

typedef struct {
	uint32_t id;
	double value;
} FloatConst;

// A block: the instructions after its label, and its terminator (NULL if the function ends first).
typedef struct {
	uint32_t label;
	size_t first;
	size_t count;
	const XscInstr *term;
} Block;

// An output the shader stores, kept as a local is till the function ends: a variable, or a member of one, gl_PerVertex.
typedef struct {
	uint32_t var;
	// The member, or XSC_NONE for the variable itself; the member's constant and the type of a pointer to it.
	int64_t member;
	uint32_t member_id;
	uint32_t pointer_type;
	// The type of what it holds.
	uint32_t type;
	bool depth;
	// A member of gl_PerVertex but gl_Position, which xost doesn't export: stores into it go.
	bool ignored;
} Slot;

// An access chain into a local vector or an output: the local or slot, and the component, XSC_NONE for all of it.
typedef struct {
	bool local;
	uint32_t target;
	int64_t component;
} Pointer;

// The locals' and outputs' values at one point, for an if/else's arms to start from and merge.
typedef struct {
	uint32_t *locals;
	uint32_t *slots;
} State;

// Where an if/else's arms meet: the merge block, and for its Phis the block each arm left from, the condition on data
// and its branch, or the arm a constant condition took.
typedef struct {
	uint32_t merge;
	uint32_t exit[2];
	bool known;
	bool taken;
	uint32_t condition;
	uint32_t branch;
} Meeting;

// The innermost loop a walk unrolls, or the hardware loop it's in: its merge and continue blocks, and the one of them a
// pass left for, 0 for none. A break or continue on data gates it: running, 1.0 while the pixel or vertex runs the
// pass, and alive, while it's in the loop, each 0 while every one is; what each local and output held where its pixel
// stopped in the pass; the counter where it broke, and where a continue on data left the pass. arms says how deep in
// arms its passes start. A hardware loop carries alive from 1 once a break on data clears it, or from entered, the
// running of the pass it's in, and keeps the pass's top.
typedef struct {
	uint32_t merge;
	uint32_t cont;
	uint32_t left;
	size_t arms;
	uint32_t iv;
	uint32_t running;
	uint32_t alive;
	State stopped;
	uint32_t broke_at;
	uint32_t continued_at;
	bool hardware;
	uint32_t alive_phi;
	uint32_t entered;
	State top;
} Unrolled;

// An arm of an if/else on data the walk is in: its condition, and whether it's the arm taken where that holds.
typedef struct {
	uint32_t condition;
	bool when;
} Arm;

// What a hardware loop carries a value of: a local, or an output's slot.
typedef struct {
	bool local;
	uint32_t target;
} Carry;

// The hardware loop a walk is in: its merge and continue blocks, how deep in arms it is, its break ending a pass on a
// value every vertex or pixel shares, its gating, what it carries, from its first in the module's list, its counter,
// and the hardware loop it's in.
typedef struct Hardware Hardware;
struct Hardware {
	uint32_t merge;
	uint32_t cont;
	size_t arms;
	bool has_break;
	uint32_t break_condition;
	bool break_when;
	Unrolled *gated;
	const Carry *carries;
	size_t carry_count;
	size_t carried_first;
	uint32_t counter;
	const Hardware *outer;
};

typedef struct {
	Xsc *x;
	XscModule *m;
	Block *blocks;
	size_t block_count;
	bool *func_var;
	uint32_t *locals;
	size_t local_count;
	IntConst *iconst;
	size_t iconst_count;
	size_t iconst_capacity;
	FloatConst *fconst;
	size_t fconst_count;
	size_t fconst_capacity;
	uint32_t int_tid;
	uint32_t float_tid;
	uint32_t bool_tid;
	uint32_t counter;
	XscInstr *minted;
	size_t minted_count;
	size_t minted_capacity;
	XscInstr *emitted;
	size_t emitted_count;
	size_t emitted_capacity;
	// Function variable -> the value it holds (env); 0 for none.
	uint32_t *env;
	uint32_t env_limit;
	// Each original id's last new id, for a value one block makes and another reads, as a Phi does.
	uint32_t *latest;
	Slot slots[MAX_SLOTS];
	uint32_t slot_value[MAX_SLOTS];
	size_t slot_count;
	// Each access chain's Pointer, by its original id; -1 for none.
	int32_t *pointer_of;
	Pointer *pointers;
	size_t pointer_count;
	size_t pointer_capacity;
	Meeting meeting;
	XscBranch *branches;
	size_t branch_count;
	size_t branch_capacity;
	XscMerge *merges;
	size_t merge_count;
	size_t merge_capacity;
	// How deep the walk is in arms of an if/else on data, and the innermost loop and hardware loop it's in.
	size_t arms;
	Arm *path;
	size_t path_capacity;
	// The flag the last gating select is on, and the branch of its own it has.
	uint32_t gate_flag;
	uint32_t gate_branch;
	Unrolled *unrolled;
	Hardware *hardware;
	XscHardwareLoop *loops;
	size_t loop_count;
	size_t loop_capacity;
	XscCarried *carried;
	size_t carried_count;
	size_t carried_capacity;
	uint32_t true_id;
	bool refused;
} Unroller;

static bool refuse(Unroller *u, const char *message)
{
	u->refused = true;
	return xsdk_xsc_fail(u->x, XSDK_XSC_REFUSED, "%s", message);
}

static bool is_func_var(const Unroller *u, uint32_t id)
{
	return id < u->m->id_limit && u->func_var[id];
}

static bool find_int(const Unroller *u, uint32_t id, int64_t *value)
{
	for (size_t i = 0; i < u->iconst_count; i++) {
		if (u->iconst[i].id == id) {
			*value = u->iconst[i].value;
			return true;
		}
	}
	return false;
}

static bool find_float(const Unroller *u, uint32_t id, double *value)
{
	for (size_t i = 0; i < u->fconst_count; i++) {
		if (u->fconst[i].id == id) {
			*value = u->fconst[i].value;
			return true;
		}
	}
	return false;
}

static uint32_t env_get(const Unroller *u, uint32_t id)
{
	return id < u->env_limit ? u->env[id] : 0;
}

// The value an id stands for: a local map's, the environment's, or the id itself (unroll.py remap's rr).
typedef struct {
	uint32_t *from;
	uint32_t *to;
	size_t count;
	size_t capacity;
} LocalMap;

static bool map_get(const LocalMap *lm, uint32_t id, uint32_t *value)
{
	for (size_t i = lm->count; i > 0; i--) {
		if (lm->from[i - 1] == id) {
			*value = lm->to[i - 1];
			return true;
		}
	}
	return false;
}

static bool map_set(Unroller *u, LocalMap *lm, uint32_t id, uint32_t value)
{
	size_t capacity = lm->capacity;
	if (!xsdk_xsc_room(u->x, &lm->from, lm->count, &capacity, sizeof *lm->from))
		return false;
	capacity = lm->capacity;
	if (!xsdk_xsc_room(u->x, &lm->to, lm->count, &capacity, sizeof *lm->to))
		return false;
	lm->capacity = capacity;
	lm->from[lm->count] = id;
	lm->to[lm->count] = value;
	lm->count++;
	if (id < u->env_limit)
		u->latest[id] = value;
	return true;
}

static uint32_t through(const Unroller *u, const LocalMap *lm, uint32_t id)
{
	uint32_t value;
	if (map_get(lm, id, &value))
		return value;
	const uint32_t held = env_get(u, id);
	if (held)
		return held;
	return id < u->env_limit && u->latest[id] ? u->latest[id] : id;
}

static uint32_t local_or_self(const LocalMap *lm, uint32_t id)
{
	uint32_t value;
	return map_get(lm, id, &value) ? value : id;
}

static uint32_t fresh(Unroller *u)
{
	return u->counter++;
}

static bool add_instr(Unroller *u, XscInstr **list, size_t *count, size_t *capacity, XscInstr ins)
{
	if (!xsdk_xsc_room(u->x, list, *count, capacity, sizeof **list))
		return false;
	(*list)[(*count)++] = ins;
	return true;
}

static uint32_t *new_words(Unroller *u, size_t count)
{
	return xsdk_xsc_alloc(u->x, (count ? count : 1) * sizeof(uint32_t));
}

// A new value instruction of the given words after its result type and id.
static bool emit_value(
	Unroller *u, uint32_t opcode, uint32_t type, const uint32_t *operands, uint32_t count, uint32_t *id)
{
	uint32_t *const words = new_words(u, (size_t)count + 2);
	if (!words)
		return false;
	*id = fresh(u);
	words[0] = type;
	words[1] = *id;
	if (count)
		memcpy(words + 2, operands, count * sizeof *operands);
	return add_instr(
		u, &u->emitted, &u->emitted_count, &u->emitted_capacity, (XscInstr){ opcode, type, *id, words, count + 2 });
}

static bool int_id(Unroller *u, int64_t value, uint32_t *id)
{
	for (size_t i = 0; i < u->iconst_count; i++) {
		if (u->iconst[i].value == value) {
			*id = u->iconst[i].id;
			return true;
		}
	}
	if (!u->int_tid)
		return refuse(u, "no 32-bit int type to materialise a constant");
	const uint32_t nid = fresh(u);
	uint32_t *const words = new_words(u, 3);
	if (!words)
		return false;
	words[0] = u->int_tid;
	words[1] = nid;
	words[2] = (uint32_t)value;
	if (!add_instr(u, &u->minted, &u->minted_count, &u->minted_capacity,
			(XscInstr){ SPV_CONSTANT, u->int_tid, nid, words, 3 }))
		return false;
	if (!xsdk_xsc_room(u->x, &u->iconst, u->iconst_count, &u->iconst_capacity, sizeof *u->iconst))
		return false;
	u->iconst[u->iconst_count++] = (IntConst){ nid, value };
	*id = nid;
	return true;
}

static bool float_id(Unroller *u, double value, uint32_t *id)
{
	for (size_t i = 0; i < u->fconst_count; i++) {
		if (u->fconst[i].value == value) {
			*id = u->fconst[i].id;
			return true;
		}
	}
	if (!u->float_tid)
		return refuse(u, "no 32-bit float type to materialise a constant");
	const uint32_t nid = fresh(u);
	uint32_t *const words = new_words(u, 3);
	if (!words)
		return false;
	words[0] = u->float_tid;
	words[1] = nid;
	words[2] = xsdk_xsc_bits_of_float(value);
	if (!add_instr(u, &u->minted, &u->minted_count, &u->minted_capacity,
			(XscInstr){ SPV_CONSTANT, u->float_tid, nid, words, 3 }))
		return false;
	if (!xsdk_xsc_room(u->x, &u->fconst, u->fconst_count, &u->fconst_capacity, sizeof *u->fconst))
		return false;
	u->fconst[u->fconst_count++] = (FloatConst){ nid, value };
	*id = nid;
	return true;
}

static int64_t signed_literal(uint32_t literal)
{
	return literal >= 0x80000000u ? (int64_t)literal - 0x100000000LL : literal;
}

static bool read_constants(Unroller *u)
{
	const XscModule *const m = u->m;
	bool *const int_type = xsdk_xsc_alloc(u->x, m->id_limit);
	bool *const float_type = xsdk_xsc_alloc(u->x, m->id_limit);
	if (!int_type || !float_type)
		return false;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		const uint32_t width = xsdk_xsc_operand(ins, 0);
		if (ins->opcode == SPV_TYPE_INT) {
			int_type[ins->result_id] = true;
			if (width == 32 && xsdk_xsc_operand(ins, 1) == 1 && !u->int_tid)
				u->int_tid = ins->result_id;
		} else if (ins->opcode == SPV_TYPE_FLOAT) {
			float_type[ins->result_id] = true;
			if (width == 32 && !u->float_tid)
				u->float_tid = ins->result_id;
		} else if (ins->opcode == SPV_TYPE_BOOL && !u->bool_tid) {
			u->bool_tid = ins->result_id;
		}
	}
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_CONSTANT || ins->result_type >= m->id_limit)
			continue;
		const uint32_t literal = xsdk_xsc_operand(ins, 0);
		if (int_type[ins->result_type]) {
			if (!xsdk_xsc_room(u->x, &u->iconst, u->iconst_count, &u->iconst_capacity, sizeof *u->iconst))
				return false;
			u->iconst[u->iconst_count++] = (IntConst){ ins->result_id, signed_literal(literal) };
		} else if (float_type[ins->result_type]) {
			if (!xsdk_xsc_room(u->x, &u->fconst, u->fconst_count, &u->fconst_capacity, sizeof *u->fconst))
				return false;
			u->fconst[u->fconst_count++] = (FloatConst){ ins->result_id, xsdk_xsc_float_bits(literal) };
		}
	}
	return true;
}

static int64_t int_fold(uint32_t op, int64_t a, int64_t b)
{
	if (op == OP_I_ADD)
		return a + b;
	if (op == OP_I_SUB)
		return a - b;
	if (op == OP_I_MUL)
		return a * b;
	// SPIR-V SDiv truncates toward zero, as Python's int(a / b) of the true quotient does.
	return b != 0 ? (int64_t)((double)a / (double)b) : 0;
}

static bool int_compare(uint32_t op, int64_t a, int64_t b)
{
	switch (op) {
	case 170:
		return a == b;
	case 171:
		return a != b;
	case 172:
	case 173:
		return a > b;
	case 174:
	case 175:
		return a >= b;
	case 176:
	case 177:
		return a < b;
	default:
		return a <= b;
	}
}

static bool is_compare(uint32_t op)
{
	return op >= OP_I_EQUAL && op <= OP_S_LESS_THAN_EQUAL;
}

static bool is_return(uint32_t op)
{
	return op == SPV_KILL || op == SPV_RETURN || op == SPV_RETURN_VALUE;
}

static bool is_chain(uint32_t op)
{
	return op == SPV_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN;
}

// The type a variable points at.
static uint32_t pointee(const Unroller *u, uint32_t var)
{
	const XscInstr *const v = xsdk_xsc_by_result(u->m, var);
	const XscInstr *const ptr = v ? xsdk_xsc_by_result(u->m, v->result_type) : NULL;
	return ptr && ptr->opcode == SPV_TYPE_POINTER ? xsdk_xsc_operand(ptr, 1) : 0;
}

static bool constant_int(const Unroller *u, uint32_t id, int64_t *value);

// A copy of a value instruction with its ids remapped (unroll.py remap, clone_generic).
static bool clone_generic(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	uint32_t *const words = new_words(u, (size_t)count + 2);
	if (!words)
		return false;
	const uint32_t nid = fresh(u);
	words[0] = ins->result_type;
	words[1] = nid;
	for (uint32_t i = 0; i < count; i++) {
		// An ExtInst's instruction number, a shuffle's components and an extract's or insert's indices are literals.
		bool literal = false;
		if (ins->opcode == SPV_EXT_INST)
			literal = i == 1;
		else if (ins->opcode == SPV_VECTOR_SHUFFLE)
			literal = i >= 2;
		else if (ins->opcode == SPV_COMPOSITE_EXTRACT)
			literal = i >= 1;
		else if (ins->opcode == SPV_COMPOSITE_INSERT)
			literal = i >= 2;
		words[2 + i] = literal ? ops[i] : through(u, lm, ops[i]);
	}
	if (!add_instr(u, &u->emitted, &u->emitted_count, &u->emitted_capacity,
			(XscInstr){ ins->opcode, ins->result_type, nid, words, count + 2 }))
		return false;
	return map_set(u, lm, ins->result_id, nid);
}

static bool env_set(Unroller *u, uint32_t var, uint32_t value)
{
	if (var >= u->env_limit)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "local %%%u past the module's ids", (unsigned)var);
	u->env[var] = value;
	return true;
}

static bool refuse_id(Unroller *u, const char *format, uint32_t id)
{
	char message[MESSAGE_BYTES];
	(void)snprintf(message, sizeof message, format, (unsigned)id);
	return refuse(u, message);
}

// ---- outputs and access chains ----

static bool is_output(const Unroller *u, uint32_t var)
{
	const XscInstr *const v = xsdk_xsc_by_result(u->m, var);
	return v && v->opcode == SPV_VARIABLE && xsdk_xsc_operand(v, 0) == SC_OUTPUT;
}

static bool decorated(const XscModule *m, uint32_t id, uint32_t builtin)
{
	const XscDecoration *const d = xsdk_xsc_decoration(m, id, SPV_DEC_BUILTIN);
	return d && xsdk_xsc_decoration_arg(d, XSC_NONE) == builtin;
}

// Whether member of the struct a variable points at is gl_Position.
static bool position_member(const Unroller *u, uint32_t var, int64_t member)
{
	const XscDecoration *const d = xsdk_xsc_member_decoration(u->m, pointee(u, var), (uint32_t)member, SPV_DEC_BUILTIN);
	return d && xsdk_xsc_decoration_arg(d, XSC_NONE) == BUILTIN_POSITION;
}

// The output's slot, made the first time it's reached; member is XSC_NONE for the variable itself.
static bool slot_of(Unroller *u, uint32_t var, int64_t member, uint32_t member_id, uint32_t pointer_type, size_t *slot)
{
	for (size_t i = 0; i < u->slot_count; i++) {
		if (u->slots[i].var == var && u->slots[i].member == member) {
			*slot = i;
			return true;
		}
	}
	if (u->slot_count == MAX_SLOTS)
		return refuse(u, "more outputs than a shader exports");
	uint32_t type = pointee(u, var);
	if (member != XSC_NONE) {
		const XscInstr *const st = xsdk_xsc_by_result(u->m, type);
		type = st && st->opcode == SPV_TYPE_STRUCT ? xsdk_xsc_operand(st, (uint32_t)member) : 0;
	}
	*slot = u->slot_count++;
	u->slots[*slot] = (Slot){ .var = var,
		.member = member,
		.member_id = member_id,
		.pointer_type = pointer_type,
		.type = type,
		.depth = member == XSC_NONE && decorated(u->m, var, BUILTIN_FRAG_DEPTH),
		.ignored = member != XSC_NONE && !position_member(u, var, member) };
	u->slot_value[*slot] = 0;
	return true;
}

static bool add_pointer(Unroller *u, uint32_t chain, Pointer pointer)
{
	if (chain >= u->env_limit)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "chain %%%u past the module's ids", (unsigned)chain);
	if (!xsdk_xsc_room(u->x, &u->pointers, u->pointer_count, &u->pointer_capacity, sizeof *u->pointers))
		return false;
	u->pointer_of[chain] = (int32_t)u->pointer_count;
	u->pointers[u->pointer_count++] = pointer;
	return true;
}

static const Pointer *pointer(const Unroller *u, uint32_t chain)
{
	return chain < u->env_limit && u->pointer_of[chain] >= 0 ? &u->pointers[u->pointer_of[chain]] : NULL;
}

// A chain of constant indices into a local vector, or into an output: a member of gl_PerVertex, then a component.
// done says it was one; any other chain is a value like the rest.
static bool interpret_chain(Unroller *u, const XscInstr *ins, bool *done)
{
	const uint32_t base = xsdk_xsc_operand(ins, 0);
	const uint32_t count = xsdk_xsc_operand_count(ins);
	*done = is_func_var(u, base) || is_output(u, base);
	if (!*done)
		return true;
	const XscInstr *const shape = xsdk_xsc_by_result(u->m, pointee(u, base));
	const bool vector = shape && shape->opcode == SPV_TYPE_VECTOR;
	const bool member = shape && shape->opcode == SPV_TYPE_STRUCT && !is_func_var(u, base);
	if (count < 2 || count > (member ? 3u : 2u) || (!vector && !member))
		return refuse(u, "indexed access into a local or output not supported");
	int64_t first, second = XSC_NONE;
	if (!constant_int(u, xsdk_xsc_operand(ins, 1), &first) ||
		(count > 2 && !constant_int(u, xsdk_xsc_operand(ins, 2), &second)))
		return refuse_id(u, "a run-time index into local or output %%%u", base);
	if (is_func_var(u, base))
		return add_pointer(u, ins->result_id, (Pointer){ true, base, first });
	size_t slot;
	if (!slot_of(u, base, member ? first : XSC_NONE, xsdk_xsc_operand(ins, 1), ins->result_type, &slot))
		return false;
	return add_pointer(u, ins->result_id, (Pointer){ false, (uint32_t)slot, member ? second : first });
}

// What a local or slot holds now.
static uint32_t *held(Unroller *u, const Pointer *p)
{
	return p->local ? &u->env[p->target] : &u->slot_value[p->target];
}

static bool interpret_load(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t ptr = xsdk_xsc_operand(ins, 0);
	if (is_func_var(u, ptr)) {
		if (!env_get(u, ptr))
			return refuse_id(u, "local %%%u read before write", ptr);
		return map_set(u, lm, ins->result_id, env_get(u, ptr));
	}
	const Pointer *const p = pointer(u, ptr);
	size_t slot;
	if (!p && is_output(u, ptr)) {
		if (!slot_of(u, ptr, XSC_NONE, 0, 0, &slot))
			return false;
		if (!u->slot_value[slot])
			return refuse_id(u, "output %%%u read before write", ptr);
		return map_set(u, lm, ins->result_id, u->slot_value[slot]);
	}
	if (p) {
		const uint32_t whole = *held(u, p);
		if (!whole)
			return refuse_id(u, "%%%u read before write", ptr);
		if (p->component == XSC_NONE)
			return map_set(u, lm, ins->result_id, whole);
		const uint32_t operands[2] = { whole, (uint32_t)p->component };
		uint32_t id;
		return emit_value(u, SPV_COMPOSITE_EXTRACT, ins->result_type, operands, 2, &id) &&
			map_set(u, lm, ins->result_id, id);
	}
	uint32_t *const words = new_words(u, 3);
	if (!words)
		return false;
	const uint32_t nid = fresh(u);
	words[0] = ins->result_type;
	words[1] = nid;
	words[2] = local_or_self(lm, ptr);
	if (!add_instr(u, &u->emitted, &u->emitted_count, &u->emitted_capacity,
			(XscInstr){ SPV_LOAD, ins->result_type, nid, words, 3 }))
		return false;
	return map_set(u, lm, ins->result_id, nid);
}

// The type a Pointer's whole local or slot holds.
static uint32_t held_type(const Unroller *u, const Pointer *p)
{
	return p->local ? pointee(u, p->target) : u->slots[p->target].type;
}

static bool interpret_store(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t target = xsdk_xsc_operand(ins, 0);
	const uint32_t value = through(u, lm, xsdk_xsc_operand(ins, 1));
	if (is_func_var(u, target))
		return env_set(u, target, value);
	const Pointer *const p = pointer(u, target);
	if (!p && is_output(u, target)) {
		size_t slot;
		if (!slot_of(u, target, XSC_NONE, 0, 0, &slot))
			return false;
		u->slot_value[slot] = value;
		return true;
	}
	if (p && !p->local && u->slots[p->target].ignored)
		return true;
	if (p) {
		uint32_t *const whole = held(u, p);
		if (p->component == XSC_NONE) {
			*whole = value;
			return true;
		}
		if (!*whole)
			return refuse_id(u, "part of %%%u stored before the whole of it", target);
		const uint32_t operands[3] = { value, *whole, (uint32_t)p->component };
		return emit_value(u, SPV_COMPOSITE_INSERT, held_type(u, p), operands, 3, whole);
	}
	uint32_t *const words = new_words(u, 2);
	if (!words)
		return false;
	words[0] = local_or_self(lm, target);
	words[1] = value;
	return add_instr(u, &u->emitted, &u->emitted_count, &u->emitted_capacity, (XscInstr){ SPV_STORE, 0, 0, words, 2 });
}

// ---- constants folded ----

static bool interpret_int(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t a = local_or_self(lm, xsdk_xsc_operand(ins, 0));
	const uint32_t b = local_or_self(lm, xsdk_xsc_operand(ins, 1));
	int64_t va, vb;
	if (!find_int(u, a, &va) || !find_int(u, b, &vb))
		return clone_generic(u, ins, lm);
	uint32_t id;
	if (!int_id(u, int_fold(ins->opcode, va, vb), &id))
		return false;
	return map_set(u, lm, ins->result_id, id);
}

static bool interpret_float(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t a = local_or_self(lm, xsdk_xsc_operand(ins, 0));
	double va, vb;
	const bool a_const = find_float(u, a, &va);
	uint32_t id;
	if (ins->opcode == SPV_F_NEGATE) {
		if (!a_const)
			return clone_generic(u, ins, lm);
		if (!float_id(u, -va, &id))
			return false;
		return map_set(u, lm, ins->result_id, id);
	}
	const uint32_t b = local_or_self(lm, xsdk_xsc_operand(ins, 1));
	const bool b_const = find_float(u, b, &vb);
	if (!a_const || !b_const || (ins->opcode == SPV_F_DIV && vb == 0.0))
		return clone_generic(u, ins, lm);
	double r;
	if (ins->opcode == SPV_F_ADD)
		r = va + vb;
	else if (ins->opcode == SPV_F_SUB)
		r = va - vb;
	else if (ins->opcode == SPV_F_MUL)
		r = va * vb;
	else
		r = va / vb;
	if (!float_id(u, r, &id))
		return false;
	return map_set(u, lm, ins->result_id, id);
}

static bool interpret_convert(Unroller *u, const XscInstr *ins, LocalMap *lm)
{
	const uint32_t a = local_or_self(lm, xsdk_xsc_operand(ins, 0));
	uint32_t id;
	if (ins->opcode == SPV_CONVERT_S_TO_F) {
		int64_t v;
		if (!find_int(u, a, &v))
			return clone_generic(u, ins, lm);
		if (!float_id(u, (double)v, &id))
			return false;
	} else {
		double v;
		if (!find_float(u, a, &v))
			return clone_generic(u, ins, lm);
		if (!int_id(u, (int64_t)trunc(v), &id))
			return false;
	}
	return map_set(u, lm, ins->result_id, id);
}

static const Block *block_of(const Unroller *u, uint32_t label)
{
	for (size_t i = 0; i < u->block_count; i++) {
		if (u->blocks[i].label == label)
			return &u->blocks[i];
	}
	return NULL;
}

static bool block_has(const Unroller *u, const Block *b, uint32_t opcode, const XscInstr **found)
{
	for (size_t i = b->first; i < b->first + b->count; i++) {
		if (u->m->instrs[i].opcode == opcode) {
			if (found)
				*found = &u->m->instrs[i];
			return true;
		}
	}
	return false;
}

static bool constant_int(const Unroller *u, uint32_t id, int64_t *value)
{
	if (find_int(u, id, value))
		return true;
	const XscInstr *const lx = xsdk_xsc_by_result(u->m, id);
	if (lx && lx->opcode == SPV_LOAD) {
		const uint32_t held_value = env_get(u, xsdk_xsc_operand(lx, 0));
		return held_value && find_int(u, held_value, value);
	}
	return false;
}

// A comparison folded to a constant with the current environment; false through known if it's data-dependent.
static bool const_bool(const Unroller *u, uint32_t ssa, bool *known)
{
	*known = false;
	const XscInstr *const c = xsdk_xsc_by_result(u->m, ssa);
	if (!c || !is_compare(c->opcode))
		return false;
	int64_t va, vb;
	if (!constant_int(u, xsdk_xsc_operand(c, 0), &va) || !constant_int(u, xsdk_xsc_operand(c, 1), &vb))
		return false;
	*known = true;
	return int_compare(c->opcode, va, vb);
}

// ---- if/else arms merged ----

static bool save(Unroller *u, State *s)
{
	if (!s->locals) {
		s->locals = xsdk_xsc_alloc(u->x, (u->local_count + 1) * sizeof *s->locals);
		s->slots = xsdk_xsc_alloc(u->x, MAX_SLOTS * sizeof *s->slots);
		if (!s->locals || !s->slots)
			return false;
	}
	for (size_t i = 0; i < u->local_count; i++)
		s->locals[i] = u->env[u->locals[i]];
	memcpy(s->slots, u->slot_value, sizeof u->slot_value);
	return true;
}

static void restore(Unroller *u, const State *s)
{
	for (size_t i = 0; i < u->local_count; i++)
		u->env[u->locals[i]] = s->locals[i];
	memcpy(u->slot_value, s->slots, sizeof u->slot_value);
}

// Whether instructions emitted since first work anything out, as a load, a pointer or a bool, the right of an && or
// ||, doesn't.
static bool works(const Unroller *u, size_t first)
{
	for (size_t i = first; i < u->emitted_count; i++) {
		const XscInstr *const ins = &u->emitted[i];
		const XscInstr *const type = xsdk_xsc_by_result(u->m, ins->result_type);
		const bool flag = type && type->opcode == SPV_TYPE_BOOL;
		if (ins->opcode != SPV_LOAD && ins->opcode != SPV_STORE && !is_chain(ins->opcode) && !flag)
			return true;
	}
	return false;
}

// One value where the arms meet: the same, the one an arm set where the other left it unset, or a select.
static bool merged(Unroller *u, uint32_t type, const uint32_t values[2], uint32_t *out)
{
	if (values[0] == values[1] || !values[1] || !values[0]) {
		*out = values[0] ? values[0] : values[1];
		return true;
	}
	const XscBranch *const b = &u->branches[u->branch_count - 1];
	const uint32_t operands[3] = { b->condition, values[0], values[1] };
	if (!emit_value(u, SPV_SELECT, type, operands, 3, out))
		return false;
	if (!xsdk_xsc_room(u->x, &u->merges, u->merge_count, &u->merge_capacity, sizeof *u->merges))
		return false;
	u->merges[u->merge_count++] = (XscMerge){ *out, (uint32_t)(u->branch_count - 1) };
	return true;
}

// gl_FragDepth must reach the export on every path, as nothing else gives the pixel a depth then
// (xenia:src/xenia/gpu/draw_util.cc:498-502).
static bool merge_states(Unroller *u, const State after[2])
{
	for (size_t i = 0; i < u->local_count; i++) {
		const uint32_t values[2] = { after[0].locals[i], after[1].locals[i] };
		if (!merged(u, pointee(u, u->locals[i]), values, &u->env[u->locals[i]]))
			return false;
	}
	for (size_t i = 0; i < u->slot_count; i++) {
		const uint32_t values[2] = { after[0].slots[i], after[1].slots[i] };
		if (u->slots[i].ignored)
			continue;
		if (u->slots[i].depth && values[0] != values[1] && (!values[0] || !values[1]))
			return refuse(u, "gl_FragDepth written on one path of an if/else only: the depth export needs every path");
		if (!merged(u, u->slots[i].type, values, &u->slot_value[i]))
			return false;
	}
	return true;
}

// ---- breaks and continues on data in an unrolled loop ----

// A select on a flag, which the lowering makes a cnd as it does a merge's, the flag a branch of its own with no arms;
// one value unset is the other.
static bool gate(Unroller *u, uint32_t flag, uint32_t type, uint32_t when, uint32_t otherwise, uint32_t *out)
{
	if (when == otherwise || !when || !otherwise) {
		*out = when ? when : otherwise;
		return true;
	}
	if (u->gate_flag != flag) {
		if (!xsdk_xsc_room(u->x, &u->branches, u->branch_count, &u->branch_capacity, sizeof *u->branches))
			return false;
		u->branches[u->branch_count++] = (XscBranch){ flag, false, u->counter, u->counter };
		u->gate_flag = flag;
		u->gate_branch = (uint32_t)(u->branch_count - 1);
	}
	const uint32_t operands[3] = { flag, when, otherwise };
	if (!emit_value(u, SPV_SELECT, type, operands, 3, out) ||
		!xsdk_xsc_room(u->x, &u->merges, u->merge_count, &u->merge_capacity, sizeof *u->merges))
		return false;
	u->merges[u->merge_count++] = (XscMerge){ *out, u->gate_branch };
	return true;
}

static bool logical(Unroller *u, uint32_t opcode, uint32_t a, uint32_t b, uint32_t *out)
{
	const uint32_t operands[2] = { a, b };
	return emit_value(u, opcode, u->bool_tid, operands, opcode == OP_LOGICAL_NOT ? 1 : 2, out);
}

// a && b, a flag of 0 holding for every pixel.
static bool both(Unroller *u, uint32_t a, uint32_t b, uint32_t *out)
{
	if (!a || !b) {
		*out = a ? a : b;
		return true;
	}
	return logical(u, OP_LOGICAL_AND, a, b, out);
}

// Each local but the loop's counter, and each output: cnd(flag, what the walk holds, what kept holds), into the walk's
// or into kept.
static bool gate_all(Unroller *u, const Unrolled *ul, uint32_t flag, State *kept, bool into_kept)
{
	for (size_t i = 0; i < u->local_count; i++) {
		const uint32_t v = u->locals[i];
		if (v != ul->iv &&
			!gate(u, flag, pointee(u, v), u->env[v], kept->locals[i], into_kept ? &kept->locals[i] : &u->env[v]))
			return false;
	}
	for (size_t i = 0; i < u->slot_count; i++) {
		if (!u->slots[i].ignored &&
			!gate(u, flag, u->slots[i].type, u->slot_value[i], kept->slots[i],
				into_kept ? &kept->slots[i] : &u->slot_value[i]))
			return false;
	}
	return true;
}

// The bool true: the module's, or one made.
static bool true_id(Unroller *u, uint32_t *id)
{
	for (size_t i = 0; !u->true_id && i < u->m->instr_count; i++) {
		const XscInstr *const ins = &u->m->instrs[i];
		if (ins->opcode == SPV_CONSTANT_TRUE && ins->result_type == u->bool_tid)
			u->true_id = ins->result_id;
	}
	if (!u->true_id) {
		uint32_t *const words = new_words(u, 2);
		if (!words)
			return false;
		u->true_id = fresh(u);
		words[0] = u->bool_tid;
		words[1] = u->true_id;
		if (!add_instr(u, &u->minted, &u->minted_count, &u->minted_capacity,
				(XscInstr){ SPV_CONSTANT_TRUE, u->bool_tid, u->true_id, words, 2 }))
			return false;
	}
	*id = u->true_id;
	return true;
}

static bool add_carried(Unroller *u, uint32_t type, uint32_t init, uint32_t *phi)
{
	if (!xsdk_xsc_room(u->x, &u->carried, u->carried_count, &u->carried_capacity, sizeof *u->carried) ||
		!emit_value(u, XSC_OP_LOOP_PHI, type, NULL, 0, phi))
		return false;
	u->carried[u->carried_count++] = (XscCarried){ .init = init, .phi = *phi };
	return true;
}

// A hardware loop's alive, carried from 1, or from the running of the pass it's in, made at its first break on data,
// running starting from it. A continue before that stopped values while running held 1, so a pixel that left gets the
// pass's top as its stopped copies.
static bool carry_alive(Unroller *u, Unrolled *ul)
{
	uint32_t from = ul->entered;
	if ((!from && !true_id(u, &from)) || !add_carried(u, u->bool_tid, from, &ul->alive_phi))
		return false;
	if (!ul->running) {
		ul->running = ul->alive = ul->alive_phi;
		return true;
	}
	State now = { 0 };
	if (!save(u, &now))
		return false;
	restore(u, &ul->stopped);
	if (!gate_all(u, ul, ul->alive_phi, &ul->top, false) || !save(u, &ul->stopped))
		return false;
	restore(u, &now);
	ul->alive = ul->alive_phi;
	return both(u, ul->running, ul->alive_phi, &ul->running);
}

// A break or continue on data, or one the unroller folds where some pixels have stopped already, as
// docs/scope/46-loops.md designs it: where the pixel stops, each value keeps a stopped copy,
// stopped = cnd(running && c, value now, stopped), c the arms' conditions since the pass began; then
// running = running && !c, and a break clears alive too and keeps the counter.
static bool stop_pass(Unroller *u, Unrolled *ul, bool is_break)
{
	if (!u->bool_tid)
		return refuse(u, "a break or continue on data without a bool type");
	if (is_break && ul->hardware && !ul->alive_phi && !carry_alive(u, ul))
		return false;
	uint32_t active = ul->running;
	for (size_t k = ul->arms; k < u->arms; k++) {
		uint32_t c = u->path[k].condition;
		if ((!u->path[k].when && !logical(u, OP_LOGICAL_NOT, c, 0, &c)) || !both(u, active, c, &active))
			return false;
	}
	uint32_t still;
	const uint32_t running = ul->running;
	if (!gate_all(u, ul, active, &ul->stopped, true) || !logical(u, OP_LOGICAL_NOT, active, 0, &still) ||
		!both(u, running, still, &ul->running))
		return false;
	if (is_break) {
		if (!ul->hardware && !gate(u, active, pointee(u, ul->iv), u->env[ul->iv], ul->broke_at, &ul->broke_at))
			return false;
		if (ul->alive == running)
			ul->alive = ul->running;
		else if (!both(u, ul->alive, still, &ul->alive))
			return false;
	} else {
		if (ul->continued_at && ul->continued_at != u->env[ul->iv])
			return refuse(u, "a continue on data where the loop's counter differs (v1)");
		ul->continued_at = u->env[ul->iv];
	}
	ul->left = ul->cont;
	return true;
}

// The walk reaching the loop's merge or continue block. A break or continue every running pixel takes ends the loop
// or the pass, as the unroller folds it, the pass's end keeping a pixel that stopped before as it was; one on data, or
// a break a pixel still in the loop doesn't reach, is gated, and so is any break from a hardware loop, whose passes
// the loop constant counts.
static bool leave(Unroller *u, bool is_break)
{
	Unrolled *const ul = u->unrolled;
	if (u->arms == ul->arms && (!is_break || (!ul->hardware && ul->running == ul->alive))) {
		ul->left = is_break ? ul->merge : ul->cont;
		return true;
	}
	return stop_pass(u, ul, is_break);
}

// A pass's end: each value where its pixel stopped, if one did.
static bool end_pass(Unroller *u, Unrolled *ul)
{
	if (ul->continued_at && ul->continued_at != u->env[ul->iv])
		return refuse(u, "a continue on data where the loop's counter differs (v1)");
	return !ul->running || gate_all(u, ul, ul->running, &ul->stopped, false);
}

static bool walk(Unroller *u, uint32_t start, uint32_t stop, bool has_stop, uint32_t *exit, bool *ended);

// The condition's new id; an integer comparison the interpreter folds away is made now that it's needed.
static bool condition_id(Unroller *u, const Block *b, LocalMap *lm, uint32_t *id)
{
	const uint32_t cond = xsdk_xsc_operand(b->term, 0);
	const XscInstr *const c = xsdk_xsc_by_result(u->m, cond);
	if (c && is_compare(c->opcode) && !map_get(lm, cond, id) && !clone_generic(u, c, lm))
		return false;
	*id = through(u, lm, cond);
	return true;
}

// Both arms of an if/else on data, each from the state before it, then their states merged. An arm a break or continue
// ended gives the merge nothing, as its pixels have stopped; where both arms end so, the walk ends too.
static bool data_branch(Unroller *u, const Block *b, LocalMap *lm, uint32_t merge, bool *ended)
{
	uint32_t condition;
	if (!condition_id(u, b, lm, &condition))
		return false;
	State before = { 0 };
	State after[2] = { { 0 }, { 0 } };
	uint32_t exits[2];
	bool left[2] = { false, false };
	bool work = false;
	const uint32_t first_id = u->counter;
	if (!save(u, &before))
		return false;
	for (int arm = 0; arm < 2; arm++) {
		const uint32_t label = xsdk_xsc_operand(b->term, 1 + (uint32_t)arm);
		const size_t first = u->emitted_count;
		restore(u, &before);
		exits[arm] = b->label;
		if (!xsdk_xsc_room(u->x, &u->path, u->arms, &u->path_capacity, sizeof *u->path))
			return false;
		u->path[u->arms++] = (Arm){ condition, arm == 0 };
		const bool walked = label == merge || walk(u, label, merge, true, &exits[arm], &left[arm]);
		u->arms--;
		if (!walked)
			return false;
		if (left[arm] && !(u->unrolled && u->unrolled->left))
			return refuse(u, "a discard or return inside an if/else on data");
		if (left[arm])
			u->unrolled->left = 0;
		work = work || works(u, first);
		if (!save(u, &after[arm]))
			return false;
	}
	if (!xsdk_xsc_room(u->x, &u->branches, u->branch_count, &u->branch_capacity, sizeof *u->branches))
		return false;
	u->branches[u->branch_count++] = (XscBranch){ condition, work, first_id, u->counter };
	u->meeting = (Meeting){ .merge = merge,
		.exit = { exits[0], exits[1] },
		.condition = condition,
		.branch = (uint32_t)(u->branch_count - 1) };
	if (left[0] || left[1]) {
		const int live = left[0] ? 1 : 0;
		after[1 - live] = after[live];
		u->meeting.exit[0] = u->meeting.exit[1] = exits[live];
		u->meeting.known = true;
	}
	if (left[0] && left[1]) {
		u->unrolled->left = u->unrolled->cont;
		*ended = true;
	}
	return merge_states(u, after);
}

// Whether a block does nothing but leave the hardware loop the walk is in.
static bool breaks_out(const Unroller *u, uint32_t label)
{
	const Block *const b = block_of(u, label);
	return u->hardware && b && !b->count && b->term && b->term->opcode == SPV_BRANCH &&
		xsdk_xsc_operand(b->term, 0) == u->hardware->merge;
}

// if (c) break; ending a hardware loop's pass: nothing between it and the continue block, and not inside an if/else of
// the pass's own.
static bool ends_pass(const Unroller *u, const Block *b, uint32_t merge)
{
	const Hardware *const hw = u->hardware;
	const bool when = breaks_out(u, xsdk_xsc_operand(b->term, 1));
	const Block *const after = block_of(u, merge);
	return (when || breaks_out(u, xsdk_xsc_operand(b->term, 2))) && !hw->has_break && u->arms == hw->arms &&
		xsdk_xsc_operand(b->term, when ? 2 : 1) == merge && after && !after->count && after->term &&
		after->term->opcode == SPV_BRANCH && xsdk_xsc_operand(after->term, 0) == hw->cont;
}

// Whether a value the walk made reads one that differs between vertices or pixels, or is one: a sample, a derivative,
// or what a hardware loop carries from such a value, on entry or at a pass's end.
static bool reads_varying(const Unroller *u, const bool *marked, const XscInstr *ins)
{
	if (xsdk_xsc_varies_itself(ins))
		return true;
	for (size_t i = 0; i < u->carried_count; i++) {
		const XscCarried *const c = &u->carried[i];
		if ((c->phi == ins->result_id || c->result == ins->result_id) && (marked[c->init] || marked[c->next]))
			return true;
	}
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	for (uint32_t k = 0; k < count; k++) {
		if (xsdk_xsc_id_operand(ins->opcode, k) && ops[k] < u->counter && marked[ops[k]])
			return true;
	}
	return false;
}

// Whether c differs between vertices or pixels, as the lowering marks values (xsdk_xsc_find_varying), the loop's
// values at the pass's end as the walk holds them: from an input or a local, or a gated loop's counter. What the loops
// it's in carry, and their counters, may yet vary by their passes' ends, so they're taken to.
static bool varies(Unroller *u, const Hardware *hw, uint32_t c, bool *data)
{
	for (size_t i = 0; i < hw->carry_count; i++) {
		const Carry *const k = &hw->carries[i];
		u->carried[hw->carried_first + i].next = k->local ? u->env[k->target] : u->slot_value[k->target];
	}
	bool *const marked = xsdk_xsc_alloc(u->x, u->counter);
	if (!marked)
		return false;
	for (size_t i = 0; i < u->m->instr_count; i++) {
		const XscInstr *const ins = &u->m->instrs[i];
		marked[ins->result_id] = ins->opcode == SPV_VARIABLE && xsdk_xsc_varies_itself(ins);
	}
	for (size_t i = 0; i < u->loop_count; i++)
		marked[u->loops[i].counter] = marked[u->loops[i].counter_result] = u->loops[i].alive != 0;
	for (const Hardware *h = hw->outer; h; h = h->outer) {
		marked[h->counter] = true;
		for (size_t i = 0; i < h->carry_count; i++)
			marked[u->carried[h->carried_first + i].phi] = true;
	}
	for (bool changed = true; changed;) {
		changed = false;
		for (size_t i = 0; i < u->emitted_count; i++) {
			const XscInstr *const ins = &u->emitted[i];
			if (ins->result_id && !marked[ins->result_id] && reads_varying(u, marked, ins)) {
				marked[ins->result_id] = true;
				changed = true;
			}
		}
	}
	*data = marked[c];
	return true;
}

// A break or continue on c being when, outside any arm of the pass, gated: a hardware loop's own condition.
static bool gated_leave(Unroller *u, Unrolled *ul, uint32_t c, bool when, bool is_break)
{
	if (!xsdk_xsc_room(u->x, &u->path, u->arms, &u->path_capacity, sizeof *u->path))
		return false;
	u->path[u->arms++] = (Arm){ c, when };
	const bool stopped = stop_pass(u, ul, is_break);
	u->arms--;
	ul->left = 0;
	return stopped;
}

// if (c) break; ending a pass: on a value every vertex or pixel shares, where no break or continue in the pass stopped
// one before, a predicated LOOP_END as it always was; otherwise gated, as a break on data in the pass is.
static bool hardware_break(Unroller *u, const Block *b, LocalMap *lm, bool when)
{
	Hardware *const hw = u->hardware;
	uint32_t c;
	bool data = false;
	hw->has_break = true;
	if (!condition_id(u, b, lm, &c) || (!hw->gated->running && !varies(u, hw, c, &data)))
		return false;
	if (!hw->gated->running && !data) {
		hw->break_when = when;
		hw->break_condition = c;
		return true;
	}
	return gated_leave(u, hw->gated, c, when, true);
}

// A structured if/else: on a constant condition, the taken arm alone, then the merge, unless the arm ended the walk; on
// data, both arms.
static bool branch(Unroller *u, const Block *b, LocalMap *lm, uint32_t *next, bool *ended)
{
	const XscInstr *sel = NULL;
	if (!block_has(u, b, SPV_SELECTION_MERGE, &sel))
		return refuse(u, "a branch outside a structured if/else or loop");
	const uint32_t merge = xsdk_xsc_operand(sel, 0);
	*next = merge;
	if (u->hardware && ends_pass(u, b, merge))
		return hardware_break(u, b, lm, breaks_out(u, xsdk_xsc_operand(b->term, 1)));
	bool known;
	const bool value = const_bool(u, xsdk_xsc_operand(b->term, 0), &known);
	if (!known)
		return data_branch(u, b, lm, merge, ended);
	const uint32_t taken = xsdk_xsc_operand(b->term, value ? 1 : 2);
	uint32_t exit = b->label;
	if (taken != merge && !walk(u, taken, merge, true, &exit, ended))
		return false;
	u->meeting = (Meeting){ .merge = merge, .exit = { exit, exit }, .known = true, .taken = value };
	return true;
}

// A Phi where an if/else's arms meet: the value from the block the arm taken left, or on data a select of both.
static bool interpret_phi(Unroller *u, const Block *b, const XscInstr *ins, LocalMap *lm)
{
	const Meeting *const at = &u->meeting;
	if (at->merge != b->label)
		return refuse(u, "a Phi other than where an if/else's arms meet");
	uint32_t count;
	const uint32_t *const ops = xsdk_xsc_operands(ins, &count);
	uint32_t values[2] = { 0, 0 };
	for (uint32_t i = 0; i + 1 < count; i += 2) {
		for (int arm = 0; arm < 2; arm++) {
			if (ops[i + 1] == at->exit[arm])
				values[arm] = through(u, lm, ops[i]);
		}
	}
	if (at->known)
		return map_set(u, lm, ins->result_id, values[0]);
	if (!values[0] || !values[1])
		return refuse(u, "a Phi without a value from each arm");
	const uint32_t operands[3] = { at->condition, values[0], values[1] };
	uint32_t id;
	if (!emit_value(u, SPV_SELECT, ins->result_type, operands, 3, &id) ||
		!xsdk_xsc_room(u->x, &u->merges, u->merge_count, &u->merge_capacity, sizeof *u->merges))
		return false;
	u->merges[u->merge_count++] = (XscMerge){ id, at->branch };
	return map_set(u, lm, ins->result_id, id);
}

// Symbolically executes a straight-line block (unroll.py interpret).
static bool interpret(Unroller *u, const Block *b, LocalMap *lm)
{
	for (size_t i = b->first; i < b->first + b->count; i++) {
		const XscInstr *const ins = &u->m->instrs[i];
		const uint32_t op = ins->opcode;
		bool ok = true;
		bool done;
		if (op == SPV_VARIABLE)
			continue;
		if (is_compare(op)) {
			// One that folds is read where it's used; one on data is a value like any other.
			(void)const_bool(u, ins->result_id, &done);
			ok = done || clone_generic(u, ins, lm);
		} else if (op == SPV_LOAD) {
			ok = interpret_load(u, ins, lm);
		} else if (op == SPV_STORE) {
			ok = interpret_store(u, ins, lm);
		} else if (op == SPV_PHI) {
			ok = interpret_phi(u, b, ins, lm);
		} else if (is_chain(op)) {
			ok = interpret_chain(u, ins, &done) && (done || clone_generic(u, ins, lm));
		} else if (op == OP_I_ADD || op == OP_I_SUB || op == OP_I_MUL || op == OP_S_DIV) {
			ok = interpret_int(u, ins, lm);
		} else if (op == SPV_CONVERT_S_TO_F || op == SPV_CONVERT_F_TO_S) {
			ok = interpret_convert(u, ins, lm);
		} else if (op == SPV_F_NEGATE || op == SPV_F_ADD || op == SPV_F_SUB || op == SPV_F_MUL || op == SPV_F_DIV) {
			ok = interpret_float(u, ins, lm);
		} else if (!ins->result_id) {
			continue;
		} else {
			ok = clone_generic(u, ins, lm);
		}
		if (!ok)
			return false;
	}
	return true;
}

typedef struct {
	uint32_t iv;
	int64_t bound;
	bool iv_left;
	bool body_when_true;
	uint32_t compare;
} LoopTest;

static bool loop_runs(const Unroller *u, const LoopTest *t)
{
	int64_t val = 0;
	(void)find_int(u, env_get(u, t->iv), &val);
	const bool r = t->iv_left ? int_compare(t->compare, val, t->bound) : int_compare(t->compare, t->bound, val);
	return t->body_when_true ? r : !r;
}

// The loop's test, i against a constant, i a local holding a constant on entry, for a loop the unroller unrolls;
// constant false for any other, a loop whose count is data.
static bool loop_test(Unroller *u, const XscInstr *cterm, uint32_t merge, LoopTest *t, uint32_t *body, bool *constant)
{
	const uint32_t cond = xsdk_xsc_operand(cterm, 0);
	const uint32_t t_lbl = xsdk_xsc_operand(cterm, 1);
	const uint32_t f_lbl = xsdk_xsc_operand(cterm, 2);
	if (merge != t_lbl && merge != f_lbl)
		return refuse(u, "loop exit does not target the merge block");
	*body = t_lbl == merge ? f_lbl : t_lbl;
	t->body_when_true = *body == t_lbl;
	*constant = false;
	const XscInstr *const cmp = xsdk_xsc_by_result(u->m, cond);
	if (!cmp || !is_compare(cmp->opcode))
		return true;
	t->compare = cmp->opcode;
	const uint32_t a = xsdk_xsc_operand(cmp, 0);
	const uint32_t b = xsdk_xsc_operand(cmp, 1);
	const XscInstr *const la = xsdk_xsc_by_result(u->m, a);
	const XscInstr *const lb = xsdk_xsc_by_result(u->m, b);
	if (la && la->opcode == SPV_LOAD && is_func_var(u, xsdk_xsc_operand(la, 0)) && find_int(u, b, &t->bound)) {
		t->iv = xsdk_xsc_operand(la, 0);
		t->iv_left = true;
	} else if (lb && lb->opcode == SPV_LOAD && is_func_var(u, xsdk_xsc_operand(lb, 0)) && find_int(u, a, &t->bound)) {
		t->iv = xsdk_xsc_operand(lb, 0);
		t->iv_left = false;
	} else {
		return true;
	}
	int64_t ignored;
	*constant = env_get(u, t->iv) && find_int(u, env_get(u, t->iv), &ignored);
	return true;
}

// ---- loops a block counts, kept for hardware loops ----

static bool is_block(const Unroller *u, uint32_t var)
{
	const XscInstr *const v = xsdk_xsc_by_result(u->m, var);
	const uint32_t storage = v && v->opcode == SPV_VARIABLE ? xsdk_xsc_operand(v, 0) : 0;
	return storage == SC_UNIFORM || storage == SC_PUSH_CONSTANT;
}

// i < a block's int, i a local holding a constant as the loop starts, the loop's false arm its exit; the chain to the
// int, and the constant.
static bool counted_by_block(
	const Unroller *u, const XscInstr *cterm, uint32_t merge, uint32_t *iv, const XscInstr **chain, int64_t *start)
{
	const XscInstr *const cmp = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(cterm, 0));
	if (!cmp || xsdk_xsc_operand(cterm, 2) != merge ||
		(cmp->opcode != SPV_S_LESS_THAN && cmp->opcode != OP_S_GREATER_THAN))
		return false;
	const bool left = cmp->opcode == SPV_S_LESS_THAN;
	const XscInstr *const counter = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(cmp, left ? 0 : 1));
	const XscInstr *const bound = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(cmp, left ? 1 : 0));
	if (!counter || counter->opcode != SPV_LOAD || !is_func_var(u, xsdk_xsc_operand(counter, 0)) || !bound ||
		bound->opcode != SPV_LOAD)
		return false;
	*iv = xsdk_xsc_operand(counter, 0);
	*chain = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(bound, 0));
	return *chain && is_chain((*chain)->opcode) && is_block(u, xsdk_xsc_operand(*chain, 0)) &&
		find_int(u, env_get(u, *iv), start);
}

static size_t block_index(const Unroller *u, uint32_t label)
{
	const Block *const b = block_of(u, label);
	return b ? (size_t)(b - u->blocks) : u->block_count;
}

// What the loop's blocks store, header to merge: each local, and each output stored whole, in stored; parts says an
// output's member or component is.
static bool stored_in_loop(const Unroller *u, uint32_t header, uint32_t merge, bool *stored, bool *parts)
{
	const size_t last = block_index(u, merge);
	*parts = false;
	for (size_t bi = block_index(u, header); bi < last && bi < u->block_count; bi++) {
		const Block *const b = &u->blocks[bi];
		for (size_t i = b->first; i < b->first + b->count; i++) {
			const XscInstr *const ins = &u->m->instrs[i];
			const uint32_t target = xsdk_xsc_operand(ins, 0);
			if (ins->opcode != SPV_STORE || target >= u->env_limit)
				continue;
			const XscInstr *const chain = xsdk_xsc_by_result(u->m, target);
			const uint32_t base = chain && is_chain(chain->opcode) ? xsdk_xsc_operand(chain, 0) : target;
			if (base < u->env_limit && (is_func_var(u, base) || (base == target && is_output(u, base))))
				stored[base] = true;
			else if (is_output(u, base))
				*parts = true;
		}
	}
	return true;
}

// The continue block's i = i + 1, and nothing else stored there.
static bool steps_by_one(const Unroller *u, uint32_t cont, uint32_t iv)
{
	const Block *const b = block_of(u, cont);
	size_t stores = 0;
	bool stepped = false;
	for (size_t i = 0; b && i < b->count; i++) {
		const XscInstr *const ins = &u->m->instrs[b->first + i];
		if (ins->opcode != SPV_STORE)
			continue;
		stores++;
		const XscInstr *const add = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(ins, 1));
		if (xsdk_xsc_operand(ins, 0) != iv || !add || add->opcode != OP_I_ADD)
			return false;
		for (uint32_t k = 0; k < 2; k++) {
			const XscInstr *const ld = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(add, k));
			int64_t one;
			if (ld && ld->opcode == SPV_LOAD && xsdk_xsc_operand(ld, 0) == iv &&
				find_int(u, xsdk_xsc_operand(add, 1 - k), &one) && one == 1)
				stepped = true;
		}
	}
	return stores == 1 && stepped;
}

// What the continue block's i = i + step adds, a constant past 0, where nothing else from the loop's header to its
// merge stores i; 0 otherwise.
static int64_t block_step(const Unroller *u, uint32_t header, uint32_t merge, uint32_t cont, uint32_t iv)
{
	const size_t last = block_index(u, merge);
	int64_t step = 0;
	size_t stores = 0;
	for (size_t bi = block_index(u, header); bi < last && bi < u->block_count; bi++) {
		const Block *const b = &u->blocks[bi];
		for (size_t i = b->first; i < b->first + b->count; i++) {
			const XscInstr *const ins = &u->m->instrs[i];
			if (ins->opcode != SPV_STORE || xsdk_xsc_operand(ins, 0) != iv)
				continue;
			stores++;
			const XscInstr *const add = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(ins, 1));
			for (uint32_t k = 0; b->label == cont && add && add->opcode == OP_I_ADD && k < 2; k++) {
				const XscInstr *const ld = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(add, k));
				if (ld && ld->opcode == SPV_LOAD && xsdk_xsc_operand(ld, 0) == iv)
					(void)find_int(u, xsdk_xsc_operand(add, 1 - k), &step);
			}
		}
	}
	return stores == 1 && step > 0 ? step : 0;
}

// Whether a block loads an int a block or a bare uniform holds, which the shader can't read but as a loop's count
// (xsc_lower.c, int_operand_of).
static bool loads_held_int(const Unroller *u, const Block *b)
{
	for (size_t i = b->first; i < b->first + b->count; i++) {
		const XscInstr *const ins = &u->m->instrs[i];
		const XscInstr *const type = xsdk_xsc_by_result(u->m, ins->result_type);
		if (ins->opcode != SPV_LOAD || !type || type->opcode != SPV_TYPE_INT)
			continue;
		const XscInstr *const chain = xsdk_xsc_by_result(u->m, xsdk_xsc_operand(ins, 0));
		const uint32_t base = chain && is_chain(chain->opcode) ? xsdk_xsc_operand(chain, 0) : xsdk_xsc_operand(ins, 0);
		const XscInstr *const v = xsdk_xsc_by_result(u->m, base);
		if (is_block(u, base) || (v && v->opcode == SPV_VARIABLE && xsdk_xsc_operand(v, 0) == SC_UNIFORM_CONSTANT))
			return true;
	}
	return false;
}

// Each local but the induction variable and each whole output the loop stores, and holds a value of before it, a value
// at the top of each pass; one it holds none of before is the pass's own, in own, and holds none after.
static bool carry_in(Unroller *u, uint32_t iv, const bool *stored, Carry *carries, size_t *count, bool *own)
{
	for (size_t i = 0; i < u->local_count; i++) {
		const uint32_t v = u->locals[i];
		if (v == iv)
			continue;
		own[v] = stored[v] && !u->env[v];
		if (!stored[v] || !u->env[v])
			continue;
		carries[(*count)++] = (Carry){ true, v };
		if (!add_carried(u, pointee(u, v), u->env[v], &u->env[v]))
			return false;
	}
	for (uint32_t v = 0; v < u->env_limit; v++) {
		size_t slot;
		if (!stored[v] || is_func_var(u, v))
			continue;
		if (!slot_of(u, v, XSC_NONE, 0, 0, &slot))
			return false;
		own[v] = !u->slot_value[slot];
		if (own[v])
			continue;
		carries[(*count)++] = (Carry){ false, (uint32_t)slot };
		if (!add_carried(u, u->slots[slot].type, u->slot_value[slot], &u->slot_value[slot]))
			return false;
	}
	return true;
}

// The continue block, its stores kept where flag is 0, as for a pixel that left the loop; 0 for none.
static bool step_pass(Unroller *u, const Unrolled *ul, const Block *cont, uint32_t flag, LocalMap *lm)
{
	State kept = { 0 };
	return (!flag || save(u, &kept)) && interpret(u, cont, lm) && (!flag || gate_all(u, ul, flag, &kept, false));
}

// Whether a loop tests its condition at its end, a do/while, its continue block branching back or out.
static bool tests_at_end(const Block *cont)
{
	return cont->term && cont->term->opcode == SPV_BRANCH_CONDITIONAL;
}

// The top of a hardware loop's pass whose count is data: alive carried from its first pass, so all of the pass is
// gated, and its condition, tested at the top but in a do/while, a break, the pass going on at the condition's arm
// into the loop.
static bool top_of_pass(Unroller *u, Unrolled *ul, uint32_t *first)
{
	if (!ul->alive_phi && !carry_alive(u, ul))
		return false;
	if (tests_at_end(block_of(u, ul->cont)))
		return true;
	const Block *const test = block_of(u, *first);
	const bool exit_when = xsdk_xsc_operand(test->term, 1) == ul->merge;
	LocalMap lm = { 0 };
	uint32_t c;
	if (!interpret(u, test, &lm) || !condition_id(u, test, &lm, &c) || !gated_leave(u, ul, c, exit_when, true))
		return false;
	*first = xsdk_xsc_operand(test->term, exit_when ? 2 : 1);
	return true;
}

// The end of a hardware loop's pass whose counter isn't i, its values gated: the continue block run for each pixel
// still in the loop, then a do/while's condition, a break.
static bool end_of_pass(Unroller *u, Unrolled *ul, const Block *cont)
{
	uint32_t c;
	LocalMap lm = { 0 };
	if (!step_pass(u, ul, cont, ul->alive, &lm))
		return false;
	if (!tests_at_end(cont))
		return true;
	ul->running = ul->alive;
	return condition_id(u, cont, &lm, &c) && gated_leave(u, ul, c, xsdk_xsc_operand(cont->term, 1) == ul->merge, true);
}

// A loop's alive, made in its pass after the loops nested in it carried theirs, moved to at, just past what the loop
// carries, so each loop's carried values run on from its first; theirs move on one.
static void place_alive(Unroller *u, size_t at, uint32_t phi)
{
	size_t from = at;
	while (u->carried[from].phi != phi)
		from++;
	const XscCarried alive = u->carried[from];
	memmove(&u->carried[at + 1], &u->carried[at], (from - at) * sizeof *u->carried);
	u->carried[at] = alive;
	for (size_t i = 0; i < u->loop_count; i++)
		u->loops[i].carried_first += u->loops[i].carried_first >= at && u->loops[i].carried_first <= from;
}

// The loop's pass walked once from the values at its top; then what it carries holds the loop's results. A block's int
// counts it, through chain, i, iv, from start by step, carried where it doesn't run from 0 by 1; or its count is data,
// chain NULL and iv 0.
static bool expand_hardware_loop(Unroller *u, const Block *header, uint32_t merge, uint32_t cont, uint32_t first,
	uint32_t iv, const XscInstr *chain, int32_t start, int32_t step)
{
	size_t depth = 1;
	for (const Hardware *h = u->hardware; h; h = h->outer)
		depth++;
	if (depth > XSC_LOOP_DEPTH)
		return refuse(u, "hardware loops nested more than 4 deep (xenia:src/xenia/gpu/ucode.h:131)");
	bool *const stored = xsdk_xsc_alloc(u->x, u->env_limit);
	bool *const own = xsdk_xsc_alloc(u->x, u->env_limit);
	Carry *const carries = xsdk_xsc_alloc(u->x, (u->local_count + MAX_SLOTS + 1) * sizeof *carries);
	bool parts;
	if (!stored || !own || !carries || !stored_in_loop(u, header->label, merge, stored, &parts))
		return false;
	if (parts)
		return refuse(u, "a store into part of an output inside a hardware loop (v1)");
	XscHardwareLoop loop = { .start = start, .step = step, .carried_first = u->carried_count, .first = u->counter };
	LocalMap lm = { 0 };
	size_t carry_count = 0;
	// The pass counter is i itself where a block's int counts the loop from 0 by 1; any other i is carried.
	const uint32_t counted = chain && !start && steps_by_one(u, cont, iv) ? iv : 0;
	const uint32_t counter_type = iv ? pointee(u, iv) : u->float_tid;
	if ((chain && !clone_generic(u, chain, &lm)) || !carry_in(u, counted, stored, carries, &carry_count, own) ||
		!emit_value(u, XSC_OP_LOOP_PHI, counter_type, NULL, 0, &loop.counter))
		return false;
	loop.count = chain ? local_or_self(&lm, chain->result_id) : 0;
	if (counted)
		u->env[iv] = loop.counter;
	// A pixel or vertex that doesn't run the pass the loop is in starts it out of the loop.
	Unrolled *const outer = u->unrolled;
	Hardware *const enclosing = u->hardware;
	Unrolled gated = { .merge = merge,
		.cont = cont,
		.arms = u->arms,
		.iv = counted,
		.hardware = true,
		.entered = outer ? outer->running : 0 };
	Hardware hw = { .merge = merge,
		.cont = cont,
		.arms = u->arms,
		.gated = &gated,
		.carries = carries,
		.carry_count = carry_count,
		.carried_first = loop.carried_first,
		.counter = loop.counter,
		.outer = enclosing };
	uint32_t exit;
	bool ended = false;
	if (!save(u, &gated.top) || !save(u, &gated.stopped) || (gated.entered && !carry_alive(u, &gated)))
		return false;
	u->hardware = &hw;
	u->unrolled = &gated;
	const bool walked = (chain || top_of_pass(u, &gated, &first)) && walk(u, first, cont, true, &exit, &ended);
	u->hardware = enclosing;
	u->unrolled = outer;
	if (!walked)
		return false;
	if (ended && !gated.left)
		return refuse(u, "a discard or return inside a hardware loop (v1)");
	if (!end_pass(u, &gated) || (!counted && !end_of_pass(u, &gated, block_of(u, cont))))
		return false;
	for (size_t i = 0; i < carry_count; i++) {
		XscCarried *const c = &u->carried[loop.carried_first + i];
		uint32_t *const value = carries[i].local ? &u->env[carries[i].target] : &u->slot_value[carries[i].target];
		c->next = *value;
		if (!emit_value(u, XSC_OP_LOOP_RESULT,
				carries[i].local ? pointee(u, carries[i].target) : u->slots[carries[i].target].type, NULL, 0,
				&c->result))
			return false;
		*value = c->result;
	}
	if (gated.alive_phi) {
		place_alive(u, loop.carried_first + carry_count, gated.alive_phi);
		XscCarried *const alive = &u->carried[loop.carried_first + carry_count++];
		alive->next = gated.alive;
		loop.alive = gated.alive_phi;
		if (!emit_value(u, XSC_OP_LOOP_RESULT, u->bool_tid, NULL, 0, &alive->result))
			return false;
	}
	for (uint32_t v = 0; v < u->env_limit; v++) {
		size_t slot;
		if (own[v] && is_func_var(u, v))
			u->env[v] = 0;
		else if (own[v] && slot_of(u, v, XSC_NONE, 0, 0, &slot))
			u->slot_value[slot] = 0;
	}
	if (!emit_value(u, XSC_OP_LOOP_RESULT, counter_type, NULL, 0, &loop.counter_result))
		return false;
	if (counted)
		u->env[iv] = loop.counter_result;
	loop.carried_count = carry_count;
	loop.break_condition = hw.break_condition;
	loop.break_when = hw.break_when;
	loop.end = u->counter;
	if (!xsdk_xsc_room(u->x, &u->loops, u->loop_count, &u->loop_capacity, sizeof *u->loops))
		return false;
	u->loops[u->loop_count++] = loop;
	return true;
}

// A loop whose count is data: a hardware loop of 255 passes, the most a loop constant counts, left on its condition
// (docs/scope/46-loops.md). Refused where the condition reads a block's or a bare uniform's int, so a bare uniform's
// loop takes the lowering of its own (xsc_lower_loop.c).
static bool expand_data_loop(Unroller *u, const Block *header, uint32_t merge, const Block *cont, uint32_t first)
{
	const bool at_end = tests_at_end(cont);
	const uint32_t a = at_end ? xsdk_xsc_operand(cont->term, 1) : 0;
	const uint32_t b = at_end ? xsdk_xsc_operand(cont->term, 2) : 0;
	if (at_end && !((a == header->label && b == merge) || (a == merge && b == header->label)))
		return refuse(u, "a loop's continue construct of more than one block (v1)");
	if (loads_held_int(u, at_end ? cont : block_of(u, first)))
		return refuse(u, "a loop's condition on a block's or uniform's int, other than i < b.n (v1)");
	return expand_hardware_loop(u, header, merge, cont->label, first, 0, NULL, 0, 1);
}

// Unrolls the loop whose header is header_lbl, or keeps it for a hardware loop where a block's int or data counts it;
// its merge label through merge (unroll.py expand_loop).
static bool expand_loop(Unroller *u, const Block *header, uint32_t *merge)
{
	const XscInstr *lm_ins = NULL;
	(void)block_has(u, header, SPV_LOOP_MERGE, &lm_ins);
	*merge = xsdk_xsc_operand(lm_ins, 0);
	const uint32_t continue_lbl = xsdk_xsc_operand(lm_ins, 1);
	if (!header->term || header->term->opcode != SPV_BRANCH)
		return refuse(u, "unexpected loop header terminator");
	const uint32_t first = xsdk_xsc_operand(header->term, 0);
	const Block *const cond_block = block_of(u, first);
	if (!cond_block)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "no loop condition block");
	const Block *const cont = block_of(u, continue_lbl);
	if (!cont)
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "no continue block");
	if (tests_at_end(cont))
		return expand_data_loop(u, header, *merge, cont, first);
	if (!cond_block->term || cond_block->term->opcode != SPV_BRANCH_CONDITIONAL)
		return refuse(u, "loop condition is not a BranchConditional");
	uint32_t iv;
	const XscInstr *chain;
	int64_t start;
	if (counted_by_block(u, cond_block->term, *merge, &iv, &chain, &start)) {
		const int64_t step = block_step(u, header->label, *merge, continue_lbl, iv);
		if (!step)
			return refuse(u,
				"a loop counted by a block, stepping i other than by a constant past 0 in its continue "
				"block alone (v1)");
		return expand_hardware_loop(u, header, *merge, continue_lbl, xsdk_xsc_operand(cond_block->term, 1), iv, chain,
			(int32_t)start, (int32_t)step);
	}
	LoopTest test = { 0 };
	uint32_t body = 0;
	bool constant;
	if (!loop_test(u, cond_block->term, *merge, &test, &body, &constant))
		return false;
	if (!constant)
		return expand_data_loop(u, header, *merge, cont, first);
	int trips = 0;
	Unrolled unrolled = { .merge = *merge, .cont = continue_lbl, .arms = u->arms, .iv = test.iv };
	Unrolled *const outer = u->unrolled;
	u->unrolled = &unrolled;
	while (loop_runs(u, &test)) {
		if (++trips > MAX_TRIP)
			return refuse(u, "loop trip count exceeds the unroll cap");
		const uint32_t before = env_get(u, test.iv);
		LocalMap lm = { 0 };
		uint32_t exit;
		bool ended = false;
		unrolled.left = 0;
		unrolled.continued_at = 0;
		if (!save(u, &unrolled.stopped) || !walk(u, body, continue_lbl, true, &exit, &ended))
			return false;
		if (ended && !unrolled.left)
			return refuse(u, "a discard or return inside a loop (v1)");
		if (!end_pass(u, &unrolled))
			return false;
		if (unrolled.left == *merge)
			break;
		// The continue block steps a pixel that continued and leaves one that broke as it was.
		if (!step_pass(u, &unrolled, cont, unrolled.alive, &lm))
			return false;
		unrolled.running = unrolled.alive;
		if (env_get(u, test.iv) == before)
			return refuse(u, "loop induction variable does not advance");
	}
	u->unrolled = outer;
	// After the loop, the counter of a pixel that broke is where it broke.
	return !unrolled.alive ||
		gate(u, unrolled.alive, pointee(u, test.iv), u->env[test.iv], unrolled.broke_at, &u->env[test.iv]);
}

// Walks blocks from start until stop; exit takes the last block walked, and ended whether a return ended the walk, or
// a break or continue out of the loop it unrolls, which that loop's left names.
static bool walk(Unroller *u, uint32_t start, uint32_t stop, bool has_stop, uint32_t *exit, bool *ended)
{
	uint32_t cur = start;
	*ended = false;
	while (!*ended && !(has_stop && cur == stop)) {
		const Block *const b = block_of(u, cur);
		if (!b)
			return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "%u", (unsigned)cur);
		if (block_has(u, b, SPV_LOOP_MERGE, NULL)) {
			if (!expand_loop(u, b, &cur))
				return false;
			continue;
		}
		LocalMap lm = { 0 };
		if (!interpret(u, b, &lm))
			return false;
		*exit = b->label;
		if (!b->term || is_return(b->term->opcode)) {
			*ended = true;
		} else if (b->term->opcode == SPV_BRANCH) {
			cur = xsdk_xsc_operand(b->term, 0);
		} else if (!branch(u, b, &lm, &cur, ended)) {
			return false;
		}
		const Unrolled *const ul = u->unrolled;
		if (!*ended && ul && (cur == ul->merge || (cur == ul->cont && cur != stop))) {
			if (!leave(u, cur == ul->merge))
				return false;
			*ended = true;
		}
	}
	return true;
}

static bool find_blocks(Unroller *u, size_t entry, size_t end)
{
	const XscModule *const m = u->m;
	size_t capacity = 0;
	size_t i = entry;
	while (i < end) {
		if (m->instrs[i].opcode != SPV_LABEL) {
			i++;
			continue;
		}
		// An if/else whose arms all break or continue meets at a block glslang ends with OpUnreachable.
		size_t j = i + 1;
		while (j < end && m->instrs[j].opcode != SPV_BRANCH && m->instrs[j].opcode != SPV_BRANCH_CONDITIONAL &&
			!is_return(m->instrs[j].opcode) && m->instrs[j].opcode != OP_UNREACHABLE)
			j++;
		if (!xsdk_xsc_room(u->x, &u->blocks, u->block_count, &capacity, sizeof *u->blocks))
			return false;
		u->blocks[u->block_count++] =
			(Block){ m->instrs[i].result_id, i + 1, j - i - 1, j < end ? &m->instrs[j] : NULL };
		i = j + 1;
	}
	return true;
}

static bool find_index(const XscModule *m, size_t from, size_t to, uint32_t opcode, size_t *index)
{
	for (size_t i = from; i < to; i++) {
		if (m->instrs[i].opcode == opcode) {
			*index = i;
			return true;
		}
	}
	return false;
}

static bool append_all(Unroller *u, XscInstr **list, size_t *count, size_t *capacity, const XscInstr *from, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!add_instr(u, list, count, capacity, from[i]))
			return false;
	}
	return true;
}

// Each output its last value, stored once at the end.
static bool store_outputs(Unroller *u)
{
	for (size_t i = 0; i < u->slot_count; i++) {
		const Slot *const s = &u->slots[i];
		if (!u->slot_value[i] || s->ignored)
			continue;
		uint32_t target = s->var;
		const uint32_t operands[2] = { s->var, s->member_id };
		if (s->member != XSC_NONE && !emit_value(u, SPV_ACCESS_CHAIN, s->pointer_type, operands, 2, &target))
			return false;
		uint32_t *const words = new_words(u, 2);
		if (!words)
			return false;
		words[0] = target;
		words[1] = u->slot_value[i];
		if (!add_instr(
				u, &u->emitted, &u->emitted_count, &u->emitted_capacity, (XscInstr){ SPV_STORE, 0, 0, words, 2 }))
			return false;
	}
	return true;
}

static bool find_locals(Unroller *u, size_t func_idx, size_t end_idx)
{
	const XscModule *const m = u->m;
	u->func_var = xsdk_xsc_alloc(u->x, m->id_limit);
	u->locals = xsdk_xsc_alloc(u->x, (size_t)m->id_limit * sizeof *u->locals);
	u->latest = xsdk_xsc_alloc(u->x, (size_t)m->id_limit * sizeof *u->latest);
	u->pointer_of = xsdk_xsc_alloc(u->x, (size_t)m->id_limit * sizeof *u->pointer_of);
	if (!u->func_var || !u->locals || !u->latest || !u->pointer_of)
		return false;
	for (uint32_t id = 0; id < m->id_limit; id++)
		u->pointer_of[id] = -1;
	for (size_t i = 0; i < m->instr_count; i++) {
		if (xsdk_xsc_declares_local(m, i, func_idx, end_idx)) {
			u->func_var[m->instrs[i].result_id] = true;
			u->locals[u->local_count++] = m->instrs[i].result_id;
		}
	}
	return true;
}

static bool rewrite_function(Unroller *u)
{
	XscModule *const m = u->m;
	size_t func_idx, end_idx, entry_idx;
	if (!find_index(m, 0, m->instr_count, SPV_FUNCTION, &func_idx) ||
		!find_index(m, 0, m->instr_count, SPV_FUNCTION_END, &end_idx) ||
		!find_index(m, func_idx, end_idx, SPV_LABEL, &entry_idx))
		return xsdk_xsc_fail(u->x, XSDK_XSC_MALFORMED, "a function without its parts");
	if (!find_blocks(u, entry_idx, end_idx))
		return false;
	u->env_limit = m->id_limit;
	u->env = xsdk_xsc_alloc(u->x, (size_t)m->id_limit * sizeof *u->env);
	if (!u->env || !find_locals(u, func_idx, end_idx) || !read_constants(u))
		return false;
	u->counter = m->bound;
	uint32_t exit;
	bool ended;
	if (!walk(u, m->instrs[entry_idx].result_id, 0, false, &exit, &ended) || !store_outputs(u))
		return false;
	// head | minted constants | the function's start and entry label | locals | body | the function's end
	XscInstr *list = NULL;
	size_t count = 0;
	size_t capacity = 0;
	bool ok = append_all(u, &list, &count, &capacity, m->instrs, func_idx) &&
		append_all(u, &list, &count, &capacity, u->minted, u->minted_count) &&
		append_all(u, &list, &count, &capacity, &m->instrs[func_idx], 1) &&
		append_all(u, &list, &count, &capacity, &m->instrs[entry_idx], 1);
	for (size_t i = func_idx; ok && i < end_idx; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_VARIABLE && xsdk_xsc_operand(ins, 0) == SPV_STORAGE_FUNCTION)
			ok = append_all(u, &list, &count, &capacity, ins, 1);
	}
	ok = ok && append_all(u, &list, &count, &capacity, u->emitted, u->emitted_count) &&
		append_all(u, &list, &count, &capacity, &m->instrs[end_idx], 1);
	if (!ok)
		return false;
	m->instrs = list;
	m->instr_count = count;
	m->bound = u->counter;
	m->branches = u->branches;
	m->branch_count = u->branch_count;
	m->merges = u->merges;
	m->merge_count = u->merge_count;
	m->loops = u->loops;
	m->loop_count = u->loop_count;
	m->carried = u->carried;
	m->carried_count = u->carried_count;
	return xsdk_xsc_spirv_index(u->x, m);
}

// One function, no branches, and a local stored more than once or into through an access chain.
static bool stores_locals_again(Xsc *x, const XscModule *m, bool *again)
{
	*again = false;
	size_t functions = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		functions += m->instrs[i].opcode == SPV_FUNCTION;
		if (m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL)
			return true;
	}
	if (functions != 1)
		return true;
	bool *const local = xsdk_xsc_alloc(x, m->id_limit);
	bool *const stored = xsdk_xsc_alloc(x, m->id_limit);
	if (!local || !stored)
		return false;
	for (size_t i = 0; i < m->instr_count; i++) {
		if (xsdk_xsc_declares_local(m, i, 0, m->instr_count))
			local[m->instrs[i].result_id] = true;
	}
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode != SPV_STORE)
			continue;
		const uint32_t target = xsdk_xsc_operand(&m->instrs[i], 0);
		const XscInstr *const chain = xsdk_xsc_by_result(m, target);
		if (chain && chain->opcode == SPV_ACCESS_CHAIN && xsdk_xsc_operand(chain, 0) < m->id_limit &&
			local[xsdk_xsc_operand(chain, 0)])
			*again = true;
		if (target < m->id_limit && local[target]) {
			if (stored[target])
				*again = true;
			stored[target] = true;
		}
	}
	return true;
}

// Whether a pixel shader stores gl_FragDepth more than once or inside an if/else, which the fragment lowering's selects
// don't take: a store between a selection's header and its merge label, in the module's order.
static bool depth_in_branch(Xsc *x, const XscModule *m, bool *inside)
{
	*inside = false;
	uint32_t depth = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == SPV_VARIABLE && decorated(m, m->instrs[i].result_id, BUILTIN_FRAG_DEPTH))
			depth = m->instrs[i].result_id;
	}
	if (!depth)
		return true;
	uint32_t *const open = xsdk_xsc_alloc(x, (m->instr_count + 1) * sizeof *open);
	if (!open)
		return false;
	size_t open_count = 0;
	size_t stores = 0;
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode == SPV_SELECTION_MERGE)
			open[open_count++] = xsdk_xsc_operand(ins, 0);
		while (ins->opcode == SPV_LABEL && open_count && open[open_count - 1] == ins->result_id)
			open_count--;
		if (ins->opcode == SPV_STORE && xsdk_xsc_operand(ins, 0) == depth) {
			stores++;
			*inside = *inside || open_count;
		}
	}
	*inside = *inside || stores > 1;
	return true;
}

// The unroller runs for a loop, a vertex shader's if/else, a pixel shader's gl_FragDepth under one, and locals
// stored again; a pixel shader's other if/else is the fragment lowering's.
static bool wanted(Xsc *x, const XscModule *m, bool *run)
{
	bool loop = false, branches = false, again = false, depth = false;
	for (size_t i = 0; i < m->instr_count; i++) {
		loop = loop || m->instrs[i].opcode == SPV_LOOP_MERGE;
		branches = branches || m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL;
	}
	const bool vertex = m->entry_point_count && m->entry_points[0].model == MODEL_VERTEX;
	if (!loop && !stores_locals_again(x, m, &again))
		return false;
	if (branches && !vertex && !depth_in_branch(x, m, &depth))
		return false;
	*run = loop || again || (branches && (vertex || depth));
	return true;
}

bool xsdk_xsc_unroll(Xsc *x, XscModule *m, bool *refused)
{
	*refused = false;
	bool run;
	if (!wanted(x, m, &run))
		return false;
	if (!run)
		return true;
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == SPV_SWITCH) {
			*refused = true;
			return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "a switch (v1)");
		}
	}
	Unroller u = { .x = x, .m = m };
	const bool ok = rewrite_function(&u);
	*refused = u.refused;
	return ok;
}
