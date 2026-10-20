#include <float.h>
#include <math.h>
#include <string.h>

#include "xsc_private.h"

// The optimiser, as xenos-toolchain/xasm/optimize.py rewrites a program's instructions before allocation: common
// subexpressions, copies propagated, identities with a known 0 or 1 and clamps a value's range makes useless folded,
// multiply-adds fused, values computed into their copies' places, dead code and unused fetches removed, and scalar ops,
// and ops that write one value in their scalar forms, co-issued.

enum {
	ALL = 0xF,
	OUTSIDE = -1,
	// At most this many different constants in an instruction (optimize.py MAX_CONSTANTS).
	MAX_CONSTANTS = 2,
	MAX_READ_KEYS = XSC_MAX_KEYS,
	MAX_DEF_KEYS = 8,
	// An op's ranges: its vector op's four lanes, then its scalar op's result.
	RANGE_SLOTS = 5,
	SCALAR_SLOT = 4,
};

typedef struct {
	bool temp;
	int64_t reg;
	bool absolute;
	int components[4];
	bool negate;
	// A constant read at a0 past its register.
	bool relative;
} Source;

// What a value may be: a number from lo to hi, NaN where nan says, and -0 where negative_zero says.
typedef struct {
	double lo;
	double hi;
	bool nan;
	bool negative_zero;
} Range;

// An instruction's reads and writes as (register, component) keys, register * 4 + component.
typedef struct {
	int64_t keys[MAX_READ_KEYS];
	size_t count;
} KeySet;

typedef struct {
	Xsc *x;
	XscProgram *p;
	XscOp **ops;
	size_t count;
	size_t ops_capacity;
	int64_t key_count;
	// The flow: each key's writers, ascending, and each read's (writer, reader) pairs, by key.
	KeySet *reads;
	KeySet *defs;
	size_t sets_capacity;
	size_t *write_start;
	size_t *write_list;
	size_t *reader_start;
	int64_t *reader_writer;
	size_t *reader_list;
	size_t *fill;
	size_t key_capacity;
	size_t write_capacity;
	size_t reader_capacity;
	// Which a0 each op reads: two ops read the same one where no a0 load or alloc lies between them.
	size_t *a0_span;
	size_t span_capacity;
	uint8_t *live;
	// Each op's results' ranges, RANGE_SLOTS an op, as fold_by_ranges finds them.
	Range *ranges;
	size_t range_capacity;
} Optimizer;

// ---- the tables (optimize.py, from xenia:src/xenia/gpu/ucode.cc:16-117) ----

static int vector_reads_table(int64_t op, int lanes[3])
{
	switch (op) {
	case XSC_VECTOR_ADD:
	case XSC_VECTOR_MUL:
	case XSC_VECTOR_MAX:
	case XSC_VECTOR_MIN:
	case XSC_VECTOR_SEQ:
	case XSC_VECTOR_SGT:
	case XSC_VECTOR_SGE:
	case XSC_VECTOR_SNE:
	case XSC_VECTOR_DP4:
	case XSC_VECTOR_CUBE:
		lanes[0] = lanes[1] = ALL;
		return 2;
	case XSC_VECTOR_FRC:
	case XSC_VECTOR_TRUNC:
	case XSC_VECTOR_FLOOR:
	case XSC_VECTOR_MAX4:
		lanes[0] = ALL;
		return 1;
	case XSC_VECTOR_MAD:
	case XSC_VECTOR_CND_EQ:
	case XSC_VECTOR_CND_GE:
	case XSC_VECTOR_CND_GT:
		lanes[0] = lanes[1] = lanes[2] = ALL;
		return 3;
	case XSC_VECTOR_DP3:
		lanes[0] = lanes[1] = 0x7;
		return 2;
	case XSC_VECTOR_DP2_ADD:
		lanes[0] = lanes[1] = 0x3;
		lanes[2] = 0x1;
		return 3;
	case XSC_VECTOR_DST:
		lanes[0] = 0x6;
		lanes[1] = 0xA;
		return 2;
	default:
		return -1;
	}
}

static bool elementwise(int64_t op)
{
	return (op >= XSC_VECTOR_ADD && op <= XSC_VECTOR_MAD) || (op >= XSC_VECTOR_CND_EQ && op <= XSC_VECTOR_CND_GT);
}

static bool vector_state(int64_t op)
{
	return (op >= XSC_VECTOR_SETP_EQ_PUSH && op <= XSC_VECTOR_KILL_NE) || op == XSC_VECTOR_MAX_A;
}

static bool with_constant(int64_t op)
{
	return op == XSC_SCALAR_MULSC || op == XSC_SCALAR_ADDSC || op == XSC_SCALAR_SUBSC;
}

// Scalar ops with one operand: 2 for a and b, 1 for a alone, 3 for b alone beside a constant, 0 for any other op.
static int scalar_one(int64_t op)
{
	if (op == XSC_SCALAR_ADDS || op == XSC_SCALAR_MULS || op == XSC_SCALAR_MAXS || op == XSC_SCALAR_MINS ||
		op == XSC_SCALAR_SUBS)
		return 2;
	if (with_constant(op))
		return 3;
	if ((op >= XSC_SCALAR_SEQS && op <= XSC_SCALAR_RSQ) || op == XSC_SCALAR_SQRT || op == XSC_SCALAR_SIN ||
		op == XSC_SCALAR_COS)
		return 1;
	return 0;
}

static bool scalar_previous(int64_t op)
{
	return op == XSC_SCALAR_ADDS_PREV || op == XSC_SCALAR_MULS_PREV || op == XSC_SCALAR_MULS_PREV2 ||
		op == XSC_SCALAR_SUBS_PREV;
}

// a0 changes only at maxa, maxas and maxasf (xenia:src/xenia/gpu/shader_interpreter.cc:600-602, 808-823).
static bool loads_a0(const XscAlu *f)
{
	return f->vector_opcode == XSC_VECTOR_MAX_A || f->scalar_opcode == XSC_SCALAR_MAX_AS ||
		f->scalar_opcode == XSC_SCALAR_MAX_ASF;
}

// ---- an instruction's fields ----

static Source source(const XscOp *op, int n)
{
	const XscAlu *const f = &op->alu;
	Source s = { .temp = f->src_is_temp[n - 1], .reg = f->src_reg[n - 1], .negate = f->src_negate[n - 1] };
	s.absolute = s.temp && (s.reg & XSC_ABS_SOURCE);
	s.relative = !s.temp && f->src_const_rel[n - 1];
	if (s.temp)
		s.reg &= XSC_ABS_SOURCE - 1;
	for (int lane = 0; lane < 4; lane++)
		s.components[lane] = (int)((f->src_swizzle[n - 1] >> (2 * lane)) + lane) & 3;
	return s;
}

// Sets operand n; the op reads its constants at a0 where an operand is one read at a0 (xsc_encode.c).
static void set_source(XscOp *op, int n, const Source *s)
{
	XscAlu *const f = &op->alu;
	f->src_reg[n - 1] = s->reg | (s->absolute ? XSC_ABS_SOURCE : 0);
	f->src_is_temp[n - 1] = s->temp;
	f->src_const_rel[n - 1] = s->relative;
	int64_t swizzle = 0;
	for (int lane = 0; lane < 4; lane++)
		swizzle += (int64_t)((s->components[lane] - lane) & 3) << (2 * lane);
	f->src_swizzle[n - 1] = swizzle;
	f->src_negate[n - 1] = s->negate;
	f->is_const_address_register_relative = false;
	for (int k = 0; k < 3; k++)
		f->is_const_address_register_relative |= !f->src_is_temp[k] && f->src_const_rel[k];
}

static bool same_value(const Source *a, const Source *b)
{
	return a->temp == b->temp && a->reg == b->reg && a->absolute == b->absolute && a->negate == b->negate &&
		a->relative == b->relative && !memcmp(a->components, b->components, sizeof a->components);
}

static bool is_alu(const XscOp *op)
{
	return op->kind == XSC_OP_ALU;
}

// Whether an op reads a constant at a run-time index, a0 past its register.
static bool addresses_constants(const XscAlu *f)
{
	return f->is_const_0_addressed || f->is_const_1_addressed || f->is_const_address_register_relative ||
		f->src_const_rel[0] || f->src_const_rel[1] || f->src_const_rel[2];
}

static int64_t vector_mask(const XscOp *op)
{
	int64_t mask = op->alu.vector_write_mask;
	if (op->alu.is_export)
		mask &= ~op->alu.scalar_write_mask;
	return mask;
}

static bool vector_active(const XscOp *op)
{
	return vector_mask(op) != 0 || vector_state(op->alu.vector_opcode);
}

static bool scalar_active(const XscOp *op)
{
	return op->alu.scalar_opcode != XSC_SCALAR_RETAIN_PREV;
}

// The lanes the vector pipe reads of operands 1 to 3, 0 for none.
static void vector_reads(const XscOp *op, int64_t lanes[4])
{
	lanes[1] = lanes[2] = lanes[3] = 0;
	if (!vector_active(op))
		return;
	int table[3];
	const int n = vector_reads_table(op->alu.vector_opcode, table);
	if (n < 0) {
		lanes[1] = lanes[2] = lanes[3] = ALL;
		return;
	}
	for (int i = 0; i < n; i++)
		lanes[i + 1] = elementwise(op->alu.vector_opcode) ? vector_mask(op) : table[i];
}

// The lanes both pipes read of each operand; an operand read for no lane by a pipe that names it still counts.
static void reads(const XscOp *op, int64_t lanes[4], bool named[4])
{
	memset(named, 0, 4 * sizeof *named);
	lanes[1] = lanes[2] = lanes[3] = 0;
	if (!is_alu(op))
		return;
	if (vector_active(op)) {
		int table[3];
		const int n = vector_reads_table(op->alu.vector_opcode, table);
		const int count = n < 0 ? 3 : n;
		vector_reads(op, lanes);
		for (int i = 1; i <= count; i++)
			named[i] = true;
	}
	if (scalar_active(op)) {
		static const int64_t read_lanes[4] = { ALL, 0x8, 0x9, 0x1 };
		lanes[3] |= read_lanes[scalar_one(op->alu.scalar_opcode)];
		named[3] = true;
	}
}

static void add_key(KeySet *set, int64_t key)
{
	for (size_t i = 0; i < set->count; i++) {
		if (set->keys[i] == key)
			return;
	}
	set->keys[set->count++] = key;
}

static bool has_key(const KeySet *set, int64_t key)
{
	for (size_t i = 0; i < set->count; i++) {
		if (set->keys[i] == key)
			return true;
	}
	return false;
}

static bool intersects(const KeySet *a, const KeySet *b)
{
	for (size_t i = 0; i < a->count; i++) {
		if (has_key(b, a->keys[i]))
			return true;
	}
	return false;
}

static int64_t key(int64_t reg, int component)
{
	return reg * 4 + component;
}

// The keys an op reads; a fetch reads its source whole (optimize.py temp_reads).
static void temp_reads(const XscOp *op, KeySet *out)
{
	out->count = 0;
	if (!is_alu(op)) {
		for (int c = 0; c < 4; c++)
			add_key(out, key(op->fetch.src, c));
		return;
	}
	int64_t lanes[4];
	bool named[4];
	reads(op, lanes, named);
	for (int n = 1; n <= 3; n++) {
		const Source s = source(op, n);
		if (!named[n] || !s.temp)
			continue;
		for (int lane = 0; lane < 4; lane++) {
			if (lanes[n] >> lane & 1)
				add_key(out, key(s.reg, s.components[lane]));
		}
	}
}

// The keys an op writes; a fetch's kept components (swizzle 7) aren't (optimize.py defs).
static void defs(const XscOp *op, KeySet *out)
{
	out->count = 0;
	if (!is_alu(op)) {
		for (int c = 0; c < 4; c++) {
			if ((op->fetch.dest_swizzle >> (3 * c) & 7) != 7)
				add_key(out, key(op->fetch.dest, c));
		}
		return;
	}
	const XscAlu *const f = &op->alu;
	if (f->is_export)
		return;
	for (int c = 0; c < 4; c++) {
		if (f->vector_write_mask >> c & 1)
			add_key(out, key(f->vector_dest, c));
	}
	for (int c = 0; c < 4; c++) {
		if (f->scalar_write_mask >> c & 1)
			add_key(out, key(f->scalar_dest, c));
	}
}

static bool constants_fit(const XscOp *op)
{
	int64_t lanes[4];
	bool named[4];
	reads(op, lanes, named);
	int64_t consts[4];
	bool has[4] = { false, false, false, false };
	int64_t distinct[3];
	int distinct_count = 0;
	int count = 0;
	for (int n = 1; n <= 3; n++) {
		const Source s = source(op, n);
		// mulsc's constant is operand 3's (xenia:src/xenia/gpu/shader_interpreter.cc:655-670).
		const bool constant = n == 3 && with_constant(op->alu.scalar_opcode);
		if (!named[n] || (s.temp && !constant))
			continue;
		// A constant read at a0 is another constant; the second and third share their addressing (ucode.h:1945-1964).
		const int64_t reg = 2 * (constant ? op->alu.scalar_constant : s.reg) + s.relative;
		consts[n] = reg;
		has[n] = true;
		count++;
		bool seen = false;
		for (int i = 0; i < distinct_count; i++)
			seen = seen || distinct[i] == reg;
		if (!seen)
			distinct[distinct_count++] = reg;
	}
	if (distinct_count > MAX_CONSTANTS)
		return false;
	return count < 3 || (has[2] && has[3] && consts[2] == consts[3]);
}

static bool fixed(const XscOp *op)
{
	if (!is_alu(op))
		return true;
	return vector_state(op->alu.vector_opcode) || (scalar_active(op) && !scalar_one(op->alu.scalar_opcode));
}

static bool is_move(const XscOp *op)
{
	if (fixed(op) || op->alu.is_export || scalar_active(op))
		return false;
	if (op->alu.vector_opcode != XSC_VECTOR_MAX || op->alu.vector_clamp || !op->alu.vector_write_mask)
		return false;
	const Source a = source(op, 1);
	const Source b = source(op, 2);
	return same_value(&a, &b);
}

static bool plain_vector(const XscOp *op)
{
	return !fixed(op) && !op->alu.is_export && !scalar_active(op) && op->alu.vector_write_mask != 0;
}

// ---- the program ----

static bool flatten(Optimizer *o)
{
	o->count = 0;
	for (size_t si = 0; si < o->p->stage_count; si++) {
		for (size_t i = 0; i < o->p->stages[si].count; i++) {
			if (!xsdk_xsc_room(o->x, &o->ops, o->count, &o->ops_capacity, sizeof *o->ops))
				return false;
			o->ops[o->count++] = o->p->stages[si].ops[i];
		}
	}
	return true;
}

static void remove_ops(Optimizer *o, XscOp *const *doomed, size_t doomed_count)
{
	for (size_t si = 0; si < o->p->stage_count; si++) {
		XscStage *const stage = &o->p->stages[si];
		size_t kept = 0;
		for (size_t i = 0; i < stage->count; i++) {
			bool gone = false;
			for (size_t d = 0; d < doomed_count; d++)
				gone = gone || stage->ops[i] == doomed[d];
			if (!gone)
				stage->ops[kept++] = stage->ops[i];
		}
		stage->count = kept;
	}
}

static bool grow(Xsc *x, void *items, size_t *capacity, size_t needed, size_t item_bytes)
{
	while (*capacity < needed) {
		if (!xsdk_xsc_room(x, items, *capacity, capacity, item_bytes))
			return false;
	}
	return true;
}

// Each op's a0: a new one past each a0 load, and past each alloc, where a0 is lost (docs/HARDWARE.md, "a0 doesn't
// survive an alloc").
static bool find_a0_spans(Optimizer *o)
{
	size_t capacity = o->span_capacity;
	if (!grow(o->x, &o->a0_span, &capacity, o->count + 1, sizeof *o->a0_span))
		return false;
	o->span_capacity = capacity;
	size_t span = 0, at = 0;
	for (size_t si = 0; si < o->p->stage_count; si++) {
		for (size_t i = 0; i < o->p->stages[si].count; i++, at++) {
			o->a0_span[at] = span;
			span += is_alu(o->p->stages[si].ops[i]) && loads_a0(&o->p->stages[si].ops[i]->alu);
		}
		span += o->p->stages[si].alloc_after != XSC_NONE;
	}
	return true;
}

// The flow from each instruction's reads and defs as they stand.
static bool index_flow(Optimizer *o)
{
	const size_t n = o->count;
	const size_t keys = (size_t)o->key_count;
	size_t key_capacity = o->key_capacity;
	bool ok = grow(o->x, &o->write_start, &key_capacity, keys + 1, sizeof *o->write_start);
	key_capacity = o->key_capacity;
	ok = ok && grow(o->x, &o->reader_start, &key_capacity, keys + 1, sizeof *o->reader_start);
	key_capacity = o->key_capacity;
	ok = ok && grow(o->x, &o->fill, &key_capacity, keys + 1, sizeof *o->fill);
	if (!ok)
		return false;
	o->key_capacity = key_capacity;
	memset(o->write_start, 0, (keys + 1) * sizeof *o->write_start);
	memset(o->reader_start, 0, (keys + 1) * sizeof *o->reader_start);
	size_t total_defs = 0, total_reads = 0;
	for (size_t i = 0; i < n; i++) {
		for (size_t k = 0; k < o->defs[i].count; k++)
			o->write_start[o->defs[i].keys[k] + 1]++;
		for (size_t k = 0; k < o->reads[i].count; k++)
			o->reader_start[o->reads[i].keys[k] + 1]++;
		total_defs += o->defs[i].count;
		total_reads += o->reads[i].count;
	}
	size_t capacity = o->write_capacity;
	ok = grow(o->x, &o->write_list, &capacity, total_defs + 1, sizeof *o->write_list);
	o->write_capacity = capacity;
	capacity = o->reader_capacity;
	ok = ok && grow(o->x, &o->reader_list, &capacity, total_reads + 1, sizeof *o->reader_list);
	capacity = o->reader_capacity;
	ok = ok && grow(o->x, &o->reader_writer, &capacity, total_reads + 1, sizeof *o->reader_writer);
	if (!ok)
		return false;
	o->reader_capacity = capacity;
	for (size_t k = 0; k < keys; k++) {
		o->write_start[k + 1] += o->write_start[k];
		o->reader_start[k + 1] += o->reader_start[k];
	}
	memcpy(o->fill, o->write_start, keys * sizeof *o->fill);
	for (size_t i = 0; i < n; i++) {
		for (size_t k = 0; k < o->defs[i].count; k++)
			o->write_list[o->fill[o->defs[i].keys[k]]++] = i;
	}
	return find_a0_spans(o);
}

// Op i's reads and defs, found again as compute_in_place changes it, so the flow needn't be built again.
static void find_keys(Optimizer *o, size_t i)
{
	temp_reads(o->ops[i], &o->reads[i]);
	defs(o->ops[i], &o->defs[i]);
}

// Which instruction's write each key holds at each instruction, and each write's readers (optimize.py _Flow).
static bool build_flow(Optimizer *o)
{
	if (!flatten(o))
		return false;
	size_t sets = o->sets_capacity;
	if (!grow(o->x, &o->reads, &sets, o->count + 1, sizeof *o->reads))
		return false;
	sets = o->sets_capacity;
	if (!grow(o->x, &o->defs, &sets, o->count + 1, sizeof *o->defs))
		return false;
	o->sets_capacity = sets;
	for (size_t i = 0; i < o->count; i++)
		find_keys(o, i);
	return index_flow(o);
}

// The write of key that op i reads (OUTSIDE if none).
static int64_t flow_at(const Optimizer *o, size_t i, int64_t k)
{
	if (k < 0 || k >= o->key_count)
		return OUTSIDE;
	int64_t found = OUTSIDE;
	for (size_t w = o->write_start[k]; w < o->write_start[k + 1]; w++) {
		if (o->write_list[w] < i)
			found = (int64_t)o->write_list[w];
	}
	return found;
}

static void fill_readers(Optimizer *o)
{
	memcpy(o->fill, o->reader_start, (size_t)o->key_count * sizeof *o->fill);
	for (size_t i = 0; i < o->count; i++) {
		for (size_t k = 0; k < o->reads[i].count; k++) {
			const int64_t rk = o->reads[i].keys[k];
			const size_t at = o->fill[rk]++;
			o->reader_writer[at] = flow_at(o, i, rk);
			o->reader_list[at] = i;
		}
	}
}

// The readers of writer w's key, in the order they were found.
static size_t readers_of(const Optimizer *o, int64_t w, int64_t k, size_t *out, size_t out_count)
{
	if (k < 0 || k >= o->key_count)
		return out_count;
	for (size_t at = o->reader_start[k]; at < o->reader_start[k + 1]; at++) {
		if (o->reader_writer[at] == w)
			out[out_count++] = o->reader_list[at];
	}
	return out_count;
}

// Whether s holds the same at ops i and j: a temp's components have the same writers, a constant at a0 the same a0.
static bool same_values(const Optimizer *o, const Source *s, const int *components, int count, size_t i, size_t j)
{
	if (!s->temp)
		return !s->relative || o->a0_span[i] == o->a0_span[j];
	for (int c = 0; c < count; c++) {
		if (flow_at(o, i, key(s->reg, components[c])) != flow_at(o, j, key(s->reg, components[c])))
			return false;
	}
	return true;
}

// The components a source reads for the lanes given, deduplicated as a set.
static int component_set(const Source *s, int64_t lanes_mask, int *out)
{
	int count = 0;
	for (int lane = 0; lane < 4; lane++) {
		if (!(lanes_mask >> lane & 1))
			continue;
		bool seen = false;
		for (int i = 0; i < count; i++)
			seen = seen || out[i] == s->components[lane];
		if (!seen)
			out[count++] = s->components[lane];
	}
	return count;
}

// ---- the passes; each returns whether it changed the program ----

// A vertex or texture fetch none of whose writes is live; the other fetches set state or are left as they are.
static bool unused_fetch(const Optimizer *o, const XscOp *op)
{
	if (is_alu(op) || (op->kind == XSC_OP_TFETCH && op->fetch.opcode != XSC_FETCH_TEXTURE))
		return false;
	KeySet set;
	defs(op, &set);
	for (size_t k = 0; k < set.count; k++) {
		if (o->live[set.keys[k]])
			return false;
	}
	return true;
}

static bool remove_dead(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!flatten(o))
		return false;
	memset(o->live, 0, (size_t)o->key_count);
	XscOp **const doomed = xsdk_xsc_alloc(o->x, (o->count + 1) * sizeof *doomed);
	if (!doomed)
		return false;
	size_t doomed_count = 0;
	for (size_t idx = o->count; idx-- > 0;) {
		XscOp *const op = o->ops[idx];
		if (is_alu(op) && !op->alu.is_export && !fixed(op)) {
			XscAlu *const f = &op->alu;
			int64_t *const masks[2] = { &f->vector_write_mask, &f->scalar_write_mask };
			const int64_t dests[2] = { f->vector_dest, f->scalar_dest };
			for (int pipe = 0; pipe < 2; pipe++) {
				int64_t keep = 0;
				for (int c = 0; c < 4; c++) {
					const int64_t k = key(dests[pipe], c);
					if ((*masks[pipe] >> c & 1) && k < o->key_count && o->live[k])
						keep |= (int64_t)1 << c;
				}
				if (keep != *masks[pipe]) {
					*masks[pipe] = keep;
					*changed = true;
				}
			}
			if (!f->scalar_write_mask && scalar_active(op)) {
				f->scalar_opcode = XSC_SCALAR_RETAIN_PREV;
				*changed = true;
			}
			if (!vector_active(op) && !scalar_active(op)) {
				doomed[doomed_count++] = op;
				continue;
			}
		} else if (unused_fetch(o, op)) {
			doomed[doomed_count++] = op;
			continue;
		}
		KeySet set;
		defs(op, &set);
		for (size_t k = 0; k < set.count; k++)
			o->live[set.keys[k]] = 0;
		temp_reads(op, &set);
		for (size_t k = 0; k < set.count; k++)
			o->live[set.keys[k]] = 1;
	}
	remove_ops(o, doomed, doomed_count);
	*changed = *changed || doomed_count;
	return true;
}

// A temp's component as op i reads it: the write it holds, an input's where none, or a constant a move copied there.
typedef struct {
	bool constant;
	int64_t writer;
	int64_t reg;
	int component;
} Value;

// What op i reads in component c of temp reg, through moves: max(x, x) is x, a NaN too, as any op reads it
// (xenia:src/xenia/gpu/shader_interpreter.cc:347-354).
static Value value_of(const Optimizer *o, size_t i, int64_t reg, int c)
{
	for (;;) {
		const int64_t w = flow_at(o, i, key(reg, c));
		const Value written = { .writer = w, .reg = reg, .component = c };
		if (w == OUTSIDE || !is_move(o->ops[w]))
			return written;
		const Source s = source(o->ops[w], 1);
		if (s.negate || s.absolute || s.relative)
			return written;
		if (!s.temp)
			return (Value){ .constant = true, .reg = s.reg, .component = s.components[c] };
		i = (size_t)w;
		reg = s.reg;
		c = s.components[c];
	}
}

// Whether temps sa at op i and sb at op j hold the same in every lane given, though moves put them in two registers.
static bool same_lanes(const Optimizer *o, const Source *sa, const Source *sb, int64_t lanes, size_t i, size_t j)
{
	for (int lane = 0; lane < 4; lane++) {
		if (!(lanes >> lane & 1))
			continue;
		const Value va = value_of(o, i, sa->reg, sa->components[lane]);
		const Value vb = value_of(o, j, sb->reg, sb->components[lane]);
		if (va.constant != vb.constant || va.writer != vb.writer || va.reg != vb.reg || va.component != vb.component)
			return false;
	}
	return true;
}

static bool same_computation(const Optimizer *o, const XscOp *a, const XscOp *b, size_t i, size_t j)
{
	const int64_t op = b->alu.vector_opcode;
	if (a->alu.vector_opcode != op || a->alu.vector_clamp != b->alu.vector_clamp)
		return false;
	int64_t lanes[4];
	vector_reads(b, lanes);
	int table[3];
	const int n = vector_reads_table(op, table);
	const int count = !vector_active(b) ? 0 : n < 0 ? 3 : n;
	for (int k = 1; k <= count; k++) {
		const Source sa = source(a, k);
		const Source sb = source(b, k);
		if (sa.temp != sb.temp || sa.absolute != sb.absolute || sa.negate != sb.negate || sa.relative != sb.relative)
			return false;
		if (sa.temp) {
			if (!same_lanes(o, &sa, &sb, lanes[k], i, j))
				return false;
			continue;
		}
		const int64_t used = elementwise(op) ? lanes[k] : ALL;
		for (int lane = 0; lane < 4; lane++) {
			if ((used >> lane & 1) && sa.components[lane] != sb.components[lane])
				return false;
		}
		if (sa.reg != sb.reg || !same_values(o, &sa, NULL, 0, i, j))
			return false;
	}
	return true;
}

static bool common_subexpressions(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	for (size_t j = 0; j < o->count; j++) {
		XscOp *const b = o->ops[j];
		if (!plain_vector(b) || is_move(b))
			continue;
		for (size_t i = j; i-- > 0;) {
			const XscOp *const a = o->ops[i];
			if (!plain_vector(a) || !same_computation(o, a, b, i, j))
				continue;
			if (b->alu.vector_write_mask & ~a->alu.vector_write_mask)
				continue;
			// a's result must still be in its register when b runs.
			bool kept = true;
			for (int c = 0; c < 4; c++) {
				if ((b->alu.vector_write_mask >> c & 1) && flow_at(o, j, key(a->alu.vector_dest, c)) != (int64_t)i)
					kept = false;
			}
			if (!kept)
				continue;
			*changed = true;
			if (a->alu.vector_dest == b->alu.vector_dest) {
				XscOp *doomed = b;
				remove_ops(o, &doomed, 1);
				return true;
			}
			const Source move = { .temp = true, .reg = a->alu.vector_dest, .components = { 0, 1, 2, 3 } };
			b->alu.vector_opcode = XSC_VECTOR_MAX;
			b->alu.vector_clamp = false;
			set_source(b, 1, &move);
			set_source(b, 2, &move);
			return true;
		}
	}
	return true;
}

static bool rewrite_reader(Optimizer *o, size_t i, const XscOp *mov, size_t r)
{
	XscOp *const reader = o->ops[r];
	if (fixed(reader))
		return false;
	const Source src = source(mov, 1);
	const int64_t dest = mov->alu.vector_dest;
	const int64_t mask = mov->alu.vector_write_mask;
	int64_t lanes[4];
	bool named[4];
	reads(reader, lanes, named);
	Source fresh[4];
	bool rewritten[4] = { false, false, false, false };
	bool any = false;
	for (int n = 1; n <= 3; n++) {
		const Source old = source(reader, n);
		if (!named[n] || !old.temp || old.reg != dest)
			continue;
		// Every lane of this operand that's read must come from the move.
		for (int lane = 0; lane < 4; lane++) {
			if (!(lanes[n] >> lane & 1))
				continue;
			if (!(mask >> old.components[lane] & 1) || flow_at(o, r, key(dest, old.components[lane])) != (int64_t)i)
				return false;
		}
		Source candidate = { .temp = src.temp, .reg = src.reg, .relative = src.relative };
		for (int lane = 0; lane < 4; lane++)
			candidate.components[lane] = src.components[(lanes[n] >> lane & 1) ? old.components[lane] : 0];
		if (old.absolute) {
			if (!src.temp)
				return false;
			candidate.absolute = true;
			candidate.negate = old.negate;
		} else {
			candidate.absolute = src.absolute;
			candidate.negate = old.negate != src.negate;
		}
		// The move's source must hold at the reader what it held at the move.
		int components[4];
		const int distinct = component_set(&candidate, lanes[n], components);
		if (!same_values(o, &candidate, components, distinct, i, r))
			return false;
		fresh[n] = candidate;
		rewritten[n] = true;
		any = true;
	}
	// An operand the op doesn't read but names as a rewritten one follows it to a temp: the allocator counts a temp
	// named and never written as an input.
	for (int m = 1; m <= 3; m++) {
		const Source s = source(reader, m);
		for (int n = 1; n <= 3 && !named[m] && !rewritten[m]; n++) {
			const Source old = source(reader, n);
			if (rewritten[n] && fresh[n].temp && same_value(&s, &old)) {
				fresh[m] = fresh[n];
				rewritten[m] = true;
			}
		}
	}
	if (!any)
		return false;
	const XscAlu saved = reader->alu;
	for (int n = 1; n <= 3; n++) {
		if (rewritten[n])
			set_source(reader, n, &fresh[n]);
	}
	if (!constants_fit(reader)) {
		reader->alu = saved;
		return false;
	}
	return true;
}

static void sort_unique(size_t *items, size_t *count)
{
	for (size_t i = 1; i < *count; i++) {
		const size_t moving = items[i];
		size_t j = i;
		for (; j > 0 && items[j - 1] > moving; j--)
			items[j] = items[j - 1];
		items[j] = moving;
	}
	size_t kept = 0;
	for (size_t i = 0; i < *count; i++) {
		if (!kept || items[kept - 1] != items[i])
			items[kept++] = items[i];
	}
	*count = kept;
}

static size_t users_of(Optimizer *o, size_t i, int64_t dest, int64_t mask, size_t *users)
{
	size_t count = 0;
	for (int c = 0; c < 4; c++) {
		if (mask >> c & 1)
			count = readers_of(o, (int64_t)i, key(dest, c), users, count);
	}
	sort_unique(users, &count);
	return count;
}

static bool propagate_copies(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	fill_readers(o);
	size_t *const users = xsdk_xsc_alloc(o->x, (4 * o->count + 1) * sizeof *users);
	if (!users)
		return false;
	for (size_t i = 0; i < o->count; i++) {
		const XscOp *const mov = o->ops[i];
		if (!is_move(mov))
			continue;
		const size_t count = users_of(o, i, mov->alu.vector_dest, mov->alu.vector_write_mask, users);
		for (size_t u = 0; u < count; u++) {
			if (rewrite_reader(o, i, mov, users[u])) {
				*changed = true;
				return true;
			}
		}
	}
	return true;
}

// ---- ranges, as Xenia's interpreter computes each op (xenia:src/xenia/gpu/shader_interpreter.cc:287-956) ----

static const Range ANY = { -INFINITY, INFINITY, true, true };

// A range, which may hold -0 wherever it reaches below 0, as a tiny negative result may become -0.
static Range range(double lo, double hi, bool nan, bool negative_zero)
{
	return (Range){ lo, hi, nan, negative_zero || lo < 0.0 };
}

static Range exactly(double value)
{
	return isnan(value) ? ANY : range(value, value, false, signbit(value) != 0);
}

// The float at or past d on the bound's side, up for an upper bound, a step further where d isn't the exact result:
// any rounding of the exact result lies within.
static double outward(double d, bool exact, bool up)
{
	if (isnan(d))
		return up ? INFINITY : -INFINITY;
	const float toward = up ? INFINITY : -INFINITY;
	float f = (float)d;
	if (up ? f < d : f > d)
		f = nextafterf(f, toward);
	return exact ? f : nextafterf(f, toward);
}

// A bound of a + b; a sum doubles round, which a fast two-sum's error term shows, isn't exact.
static double sum_bound(double a, double b, bool up)
{
	const double s = a + b;
	const bool a_bigger = fabs(a) >= fabs(b);
	const double error = (a_bigger ? b : a) - (s - (a_bigger ? a : b));
	return outward(s, !isfinite(s) || error == 0.0, up);
}

// x + y: infinities of opposite signs give NaN, and the sum is -0 only where both are.
static Range sum_range(Range x, Range y)
{
	const bool opposite = (x.hi == INFINITY && y.lo == -INFINITY) || (x.lo == -INFINITY && y.hi == INFINITY);
	return range(sum_bound(x.lo, y.lo, false), sum_bound(x.hi, y.hi, true), x.nan || y.nan || opposite,
		x.negative_zero && y.negative_zero);
}

// 0 times anything is +0 (xenia:src/xenia/gpu/shader_interpreter.cc:339-346).
static double times(double a, double b)
{
	return a == 0.0 || b == 0.0 ? 0.0 : a * b;
}

// x * y, whose ends are among its operands' ends' products, each exact in doubles.
static Range product_range(Range x, Range y)
{
	const double ends[4] = { times(x.lo, y.lo), times(x.lo, y.hi), times(x.hi, y.lo), times(x.hi, y.hi) };
	double lo = ends[0], hi = ends[0];
	for (int k = 1; k < 4; k++) {
		lo = fmin(lo, ends[k]);
		hi = fmax(hi, ends[k]);
	}
	return range(outward(lo, true, false), outward(hi, true, true), x.nan || y.nan, false);
}

// max is x >= y ? x : y, so a NaN gives y (xenia:src/xenia/gpu/shader_interpreter.cc:347-354).
static Range max_range(Range x, Range y)
{
	return range(x.nan ? y.lo : fmax(x.lo, y.lo), fmax(x.hi, y.hi), y.nan, x.negative_zero || y.negative_zero);
}

// min is x < y ? x : y (xenia:src/xenia/gpu/shader_interpreter.cc:355-361).
static Range min_range(Range x, Range y)
{
	return range(fmin(x.lo, y.lo), x.nan ? y.hi : fmin(x.hi, y.hi), y.nan, x.negative_zero || y.negative_zero);
}

// A clamp to 0 to 1 gives +0 for NaN and -0 (xenia:src/xenia/base/math.h:64-79).
static Range saturated(Range r)
{
	return range(r.nan || r.lo < 0.0 ? 0.0 : fmin(r.lo, 1.0), fmax(fmin(r.hi, 1.0), 0.0), false, false);
}

// r as a source reads it: a denormal as 0 (xenia:src/xenia/gpu/shader_interpreter.h:108-112), then its absolute
// value and negation.
static Range as_read(Range r, const Source *s)
{
	if (r.lo > 0.0 && r.lo < FLT_MIN)
		r.lo = 0.0;
	if (r.hi < 0.0 && r.hi > -FLT_MIN)
		r.hi = -0.0;
	if (s->absolute)
		r = range(r.lo >= 0.0 ? r.lo : r.hi <= 0.0 ? -r.hi : 0.0, fmax(-r.lo, r.hi), r.nan, false);
	if (s->negate)
		r = range(-r.hi, -r.lo, r.nan, r.lo <= 0.0 && r.hi >= 0.0);
	return r;
}

// Where op w keeps the range of its write of component c of reg: the scalar op's writes come after the vector op's
// (xenia:src/xenia/gpu/shader_interpreter.cc:938-956).
static size_t slot_of(const XscOp *w, int64_t reg, int c)
{
	const bool scalar = is_alu(w) && w->alu.scalar_dest == reg && (w->alu.scalar_write_mask >> c & 1);
	return scalar ? SCALAR_SLOT : (size_t)c;
}

// What source s of op i reads in a lane: a literal's number, or what an earlier op's range says. An input, a uniform
// or a texel may be any number or NaN.
static Range operand_range(const Optimizer *o, size_t i, const Source *s, int lane)
{
	const int c = s->components[lane];
	Range r = ANY;
	if (!s->temp) {
		const int64_t at = s->reg - o->p->literal_base;
		if (!s->relative && at >= 0 && (size_t)at < o->p->literal_count)
			r = exactly(o->p->literals[4 * at + c]);
	} else {
		const int64_t w = flow_at(o, i, key(s->reg, c));
		if (w != OUTSIDE)
			r = o->ranges[RANGE_SLOTS * (size_t)w + slot_of(o->ops[w], s->reg, c)];
	}
	return as_read(r, s);
}

static Range read_range(const Optimizer *o, size_t i, int n, int lane)
{
	const Source s = source(o->ops[i], n);
	return operand_range(o, i, &s, lane);
}

// The vector op's result in a lane, before its clamp; an op the ranges don't follow may give anything.
static Range vector_range(const Optimizer *o, size_t i, int lane)
{
	switch (o->ops[i]->alu.vector_opcode) {
	case XSC_VECTOR_ADD:
		return sum_range(read_range(o, i, 1, lane), read_range(o, i, 2, lane));
	case XSC_VECTOR_MUL:
		return product_range(read_range(o, i, 1, lane), read_range(o, i, 2, lane));
	case XSC_VECTOR_MAD:
		return sum_range(
			product_range(read_range(o, i, 1, lane), read_range(o, i, 2, lane)), read_range(o, i, 3, lane));
	case XSC_VECTOR_MAX:
		return max_range(read_range(o, i, 1, lane), read_range(o, i, 2, lane));
	case XSC_VECTOR_MIN:
		return min_range(read_range(o, i, 1, lane), read_range(o, i, 2, lane));
	case XSC_VECTOR_FLOOR: {
		const Range x = read_range(o, i, 1, lane);
		return range(floor(x.lo), floor(x.hi), x.nan, x.negative_zero);
	}
	default:
		return ANY;
	}
}

// The ranges of what op i writes, its clamps applied.
static void find_ranges(Optimizer *o, size_t i)
{
	const XscOp *const op = o->ops[i];
	Range *const slots = &o->ranges[RANGE_SLOTS * i];
	for (int slot = 0; slot < RANGE_SLOTS; slot++)
		slots[slot] = ANY;
	if (!is_alu(op)) {
		// A fetch's 4 and 5 are 0 and 1 (xenia:src/xenia/gpu/ucode.h:677-688).
		for (int c = 0; c < 4; c++) {
			const int64_t pick = op->fetch.dest_swizzle >> (3 * c) & 7;
			if (pick == 4 || pick == 5)
				slots[c] = exactly((double)(pick - 4));
		}
		return;
	}
	if (op->alu.scalar_clamp)
		slots[SCALAR_SLOT] = saturated(ANY);
	for (int lane = 0; lane < 4 && !op->alu.is_export; lane++) {
		if (!(op->alu.vector_write_mask >> lane & 1))
			continue;
		slots[lane] = vector_range(o, i, lane);
		if (op->alu.vector_clamp)
			slots[lane] = saturated(slots[lane]);
	}
}

// Whether operand n of op i holds one number in every lane given, the ranges say, which value takes.
static bool known(const Optimizer *o, size_t i, int n, int64_t lanes, double *value)
{
	int seen = 0;
	for (int lane = 0; lane < 4; lane++) {
		if (!(lanes >> lane & 1))
			continue;
		const Range r = read_range(o, i, n, lane);
		if (r.nan || r.lo != r.hi || (seen++ && r.lo != *value))
			return false;
		*value = r.lo;
	}
	return seen > 0;
}

// Operand 3 as an op that doesn't read it holds it (xsc_encode.c, alu_template).
static void drop_third(XscOp *op)
{
	const Source none = { .temp = true, .components = { 0, 1, 2, 3 } };
	set_source(op, 3, &none);
}

// The op becomes a copy of operand n, negated if negate.
static void become_copy(XscOp *op, int n, bool negate)
{
	Source s = source(op, n);
	s.negate = s.negate != negate;
	op->alu.vector_opcode = XSC_VECTOR_MAX;
	set_source(op, 1, &s);
	set_source(op, 2, &s);
	drop_third(op);
}

// Whether a mad with a known 1 or -1 multiplicand became an add of the other and the addend.
static bool mad_by_one(XscOp *op, const bool k[4], const double v[4])
{
	for (int n = 1; n <= 2; n++) {
		if (!k[n] || (v[n] != 1.0 && v[n] != -1.0))
			continue;
		Source other = source(op, 3 - n);
		other.negate = other.negate != (v[n] < 0.0);
		const Source addend = source(op, 3);
		op->alu.vector_opcode = XSC_VECTOR_ADD;
		set_source(op, 1, &other);
		set_source(op, 2, &addend);
		drop_third(op);
		return true;
	}
	return false;
}

// Whether op i, an add, mul or mad with a known 0, 1 or -1, became what remains. Each keeps the result but for a
// zero's sign, as the GPU's 0 times anything is +0 (xenia:src/xenia/gpu/ucode.h:976-990).
static bool fold(Optimizer *o, size_t i)
{
	XscOp *const op = o->ops[i];
	if (!is_alu(op) || fixed(op) || scalar_active(op))
		return false;
	const int64_t opcode = op->alu.vector_opcode;
	if (opcode != XSC_VECTOR_ADD && opcode != XSC_VECTOR_MUL && opcode != XSC_VECTOR_MAD)
		return false;
	int64_t lanes[4];
	vector_reads(op, lanes);
	bool k[4];
	double v[4];
	bool temps = false;
	for (int n = 1; n <= 3; n++) {
		k[n] = known(o, i, n, lanes[n], &v[n]);
		temps = temps || (lanes[n] && source(op, n).temp);
	}
	// An op on constants alone stays: compute_in_place can put it where it's copied a lane at a time, not a constant.
	if (!temps)
		return false;
	for (int n = 1; n <= 2; n++) {
		if (!k[n])
			continue;
		if (opcode == XSC_VECTOR_ADD && v[n] == 0.0)
			become_copy(op, 3 - n, false);
		else if (opcode == XSC_VECTOR_MUL && v[n] == 0.0)
			become_copy(op, n, false);
		else if (opcode == XSC_VECTOR_MUL && (v[n] == 1.0 || v[n] == -1.0))
			become_copy(op, 3 - n, v[n] < 0.0);
		else if (opcode == XSC_VECTOR_MAD && v[n] == 0.0)
			become_copy(op, 3, false);
		else
			continue;
		return true;
	}
	if (opcode != XSC_VECTOR_MAD)
		return false;
	if (k[3] && v[3] == 0.0) {
		op->alu.vector_opcode = XSC_VECTOR_MUL;
		drop_third(op);
		return true;
	}
	return mad_by_one(op, k, v);
}

// Whether a clamp keeps a value as it is: in 0 to 1, and neither NaN nor -0.
static bool in_unit(Range r)
{
	return !r.nan && !r.negative_zero && r.lo >= 0.0 && r.hi <= 1.0;
}

// Whether a max or min of x and y gives operand n, bit for bit, whatever they hold. Equal, max gives x and min y,
// which differ for -0 and +0.
static bool keeps(int64_t opcode, int n, Range x, Range y)
{
	const bool below = x.hi < y.lo || (x.hi == y.lo && y.lo != 0.0);
	const bool max = opcode == XSC_VECTOR_MAX;
	if (n == 1)
		return !x.nan && !y.nan && (max ? x.lo >= y.hi : below);
	return max ? below : x.lo >= y.hi;
}

// Whether op i, a max or min that gives one operand whatever the ranges hold, became a copy of it, or a clamped copy
// of a value a clamp keeps lost its clamp.
static bool drop_clamp(Optimizer *o, size_t i)
{
	XscOp *const op = o->ops[i];
	if (!is_alu(op) || fixed(op) || scalar_active(op) || !op->alu.vector_write_mask)
		return false;
	const int64_t opcode = op->alu.vector_opcode;
	if (opcode != XSC_VECTOR_MAX && opcode != XSC_VECTOR_MIN)
		return false;
	const Source a = source(op, 1);
	const Source b = source(op, 2);
	const bool copy = opcode == XSC_VECTOR_MAX && same_value(&a, &b);
	const int64_t lanes = vector_mask(op);
	bool first = !copy, second = !copy, unit = op->alu.vector_clamp;
	for (int lane = 0; lane < 4; lane++) {
		if (!(lanes >> lane & 1))
			continue;
		const Range x = operand_range(o, i, &a, lane);
		const Range y = operand_range(o, i, &b, lane);
		first = first && keeps(opcode, 1, x, y);
		second = second && keeps(opcode, 2, x, y);
		unit = unit && in_unit(x);
	}
	if (copy && unit)
		op->alu.vector_clamp = false;
	else if (first || second)
		become_copy(op, first ? 1 : 2, false);
	return (copy && unit) || first || second;
}

// Folds, in the order the ops run, each op the ranges of what it reads make simpler: an identity with a known 0 or 1,
// or a clamp that can't change its value. A fold changes no op's writes, so the flow holds for the next.
static bool fold_by_ranges(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	size_t capacity = o->range_capacity;
	if (!grow(o->x, &o->ranges, &capacity, RANGE_SLOTS * o->count + 1, sizeof *o->ranges))
		return false;
	o->range_capacity = capacity;
	for (size_t i = 0; i < o->count; i++) {
		while (fold(o, i) || drop_clamp(o, i))
			*changed = true;
		find_ranges(o, i);
	}
	return true;
}

// The mul's sources for the add's lanes, through the product's components (optimize.py fuse_multiply_adds).
static void fused_sources(const XscOp *mul, const Source *tsrc, int64_t lanes, Source fused[2])
{
	for (int n = 1; n <= 2; n++) {
		const Source s = source(mul, n);
		fused[n - 1] = s;
		for (int lane = 0; lane < 4; lane++)
			fused[n - 1].components[lane] = s.components[(lanes >> lane & 1) ? tsrc->components[lane] : 0];
		fused[n - 1].negate = s.negate != (n == 1 && tsrc->negate);
	}
}

// Whether the add reads the product, from that mul, in exactly one operand; which through hit.
static bool one_hit(const Optimizer *o, const XscOp *add, size_t i, size_t r, int64_t t, int64_t lanes, int *hit)
{
	int hits = 0;
	for (int n = 1; n <= 2; n++) {
		const Source s = source(add, n);
		if (!s.temp || s.reg != t)
			continue;
		bool from_mul = false;
		for (int lane = 0; lane < 4; lane++) {
			if ((lanes >> lane & 1) && flow_at(o, r, key(t, s.components[lane])) == (int64_t)i)
				from_mul = true;
		}
		if (from_mul) {
			*hit = n;
			hits++;
		}
	}
	return hits == 1;
}

static bool try_fuse(Optimizer *o, size_t i, size_t *users)
{
	const XscOp *const mul = o->ops[i];
	if (!plain_vector(mul) || mul->alu.vector_opcode != XSC_VECTOR_MUL || mul->alu.vector_clamp)
		return false;
	const int64_t t = mul->alu.vector_dest;
	const int64_t tmask = mul->alu.vector_write_mask;
	if (users_of(o, i, t, tmask, users) != 1)
		return false;
	const size_t r = users[0];
	XscOp *const add = o->ops[r];
	if (fixed(add) || scalar_active(add) || add->alu.vector_opcode != XSC_VECTOR_ADD)
		return false;
	const int64_t lanes = vector_mask(add);
	int hit;
	if (!one_hit(o, add, i, r, t, lanes, &hit))
		return false;
	const Source tsrc = source(add, hit);
	const Source other = source(add, 3 - hit);
	if (tsrc.absolute)
		return false;
	for (int lane = 0; lane < 4; lane++) {
		if ((lanes >> lane & 1) &&
			(!(tmask >> tsrc.components[lane] & 1) || flow_at(o, r, key(t, tsrc.components[lane])) != (int64_t)i))
			return false;
	}
	Source fused[2];
	fused_sources(mul, &tsrc, lanes, fused);
	// a and b must hold at the add what they held at the multiply.
	for (int n = 0; n < 2; n++) {
		int components[4];
		const int distinct = component_set(&fused[n], lanes, components);
		if (!same_values(o, &fused[n], components, distinct, i, r))
			return false;
	}
	const XscAlu saved = add->alu;
	add->alu.vector_opcode = XSC_VECTOR_MAD;
	set_source(add, 1, &fused[0]);
	set_source(add, 2, &fused[1]);
	set_source(add, 3, &other);
	if (!constants_fit(add)) {
		add->alu = saved;
		return false;
	}
	return true;
}

static bool fuse_multiply_adds(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	fill_readers(o);
	size_t *const users = xsdk_xsc_alloc(o->x, (4 * o->count + 1) * sizeof *users);
	if (!users)
		return false;
	for (size_t i = 0; i < o->count; i++) {
		if (try_fuse(o, i, users)) {
			*changed = true;
			return true;
		}
	}
	return true;
}

// An export of its vector op alone, writing no 0 or 1 of its own, which a scalar op can join.
static bool plain_export(const XscOp *op)
{
	return !fixed(op) && op->alu.is_export && !scalar_active(op) && !op->alu.scalar_write_mask &&
		!op->alu.is_scalar_dest_relative && op->alu.vector_write_mask != 0;
}

// The lanes of its register an export writes, its 0s and 1s among them (xenia:src/xenia/gpu/ucode.h:2025-2035).
static int64_t export_lanes(const XscOp *op)
{
	if (!is_alu(op) || !op->alu.is_export)
		return 0;
	return op->alu.vector_write_mask | op->alu.scalar_write_mask | (op->alu.is_scalar_dest_relative ? ALL : 0);
}

static bool vector_only(const XscOp *op)
{
	if (!plain_vector(op) && !plain_export(op))
		return false;
	int64_t lanes[4];
	vector_reads(op, lanes);
	int table[3];
	const int n = vector_reads_table(op->alu.vector_opcode, table);
	// Operand 3 is in the vector reads when the op names three, or isn't in the table.
	return !(vector_active(op) && (n < 0 || n == 3));
}

static bool scalar_only(const XscOp *op)
{
	return !fixed(op) && !op->alu.is_export && scalar_active(op) && !vector_active(op) &&
		op->alu.scalar_write_mask != 0;
}

// Whether source s reads one component, c, in every lane given.
static bool one_component(const Source *s, int64_t lanes, int *c)
{
	*c = -1;
	for (int lane = 0; lane < 4; lane++) {
		if (!(lanes >> lane & 1))
			continue;
		if (*c >= 0 && s->components[lane] != *c)
			return false;
		*c = s->components[lane];
	}
	return true;
}

// The scalar op that computes what a vector op does in a lane, or -1.
static int64_t scalar_twin(int64_t opcode)
{
	switch (opcode) {
	case XSC_VECTOR_ADD:
		return XSC_SCALAR_ADDS;
	case XSC_VECTOR_MUL:
		return XSC_SCALAR_MULS;
	case XSC_VECTOR_MAX:
		return XSC_SCALAR_MAXS;
	case XSC_VECTOR_MIN:
		return XSC_SCALAR_MINS;
	case XSC_VECTOR_FLOOR:
		return XSC_SCALAR_FLOORS;
	case XSC_VECTOR_FRC:
		return XSC_SCALAR_FRCS;
	case XSC_VECTOR_TRUNC:
		return XSC_SCALAR_TRUNCS;
	default:
		return -1;
	}
}

// A lane of constant reg other than c holding c's number bit for bit, as a literal splat's lanes do, or -1.
static int other_lane(const XscProgram *p, int64_t reg, int c)
{
	const int64_t at = reg - p->literal_base;
	for (int k = 0; at >= 0 && (size_t)at < p->literal_count && k < 4; k++) {
		if (k != c && !memcmp(&p->literals[4 * at + k], &p->literals[4 * at + c], sizeof *p->literals))
			return k;
	}
	return -1;
}

// Whether vector op op, writing one value, has a scalar form that computes it bit for bit, then form: adds to truncs,
// mulsc, addsc and subsc run as their vector ops do (xenia:src/xenia/gpu/shader_interpreter.cc:334-401, 655-761,
// 824-829) on a and b, read with one negation, so x + -y is x - y and -x + y is -x - -y.
static bool scalar_form(const XscProgram *p, const XscOp *op, XscOp *form)
{
	int64_t opcode = scalar_twin(op->alu.vector_opcode);
	const int64_t mask = op->alu.vector_write_mask;
	if (opcode < 0 || !(plain_vector(op) || plain_export(op)) || addresses_constants(&op->alu))
		return false;
	const bool unary = opcode == XSC_SCALAR_FLOORS || opcode == XSC_SCALAR_FRCS || opcode == XSC_SCALAR_TRUNCS;
	Source a = source(op, 1);
	Source b = unary ? a : source(op, 2);
	int ca, cb;
	if (!one_component(&a, mask, &ca) || !one_component(&b, mask, &cb))
		return false;
	// The forms with a constant take it as a, which add and mul allow.
	if (a.temp && !b.temp) {
		const Source s = a;
		a = b;
		b = s;
		const int c = ca;
		ca = cb;
		cb = c;
	}
	const bool constant = !a.temp && b.temp;
	const bool sub = opcode == XSC_SCALAR_ADDS && a.negate != b.negate;
	if ((constant && opcode != XSC_SCALAR_ADDS && opcode != XSC_SCALAR_MULS) || (a.negate != b.negate && !sub))
		return false;
	// Else a and b are one register's, which src3 is.
	if (constant ? b.absolute : (a.temp != b.temp || a.reg != b.reg || a.absolute != b.absolute))
		return false;
	// The GPU reads the temp for both where the lanes match (docs/HARDWARE.md, "The scalar unit's constant forms").
	if (constant && ca == cb && (ca = other_lane(p, a.reg, ca)) < 0)
		return false;
	if (sub)
		opcode = constant ? XSC_SCALAR_SUBSC : XSC_SCALAR_SUBS;
	else if (constant)
		opcode = opcode == XSC_SCALAR_MULS ? XSC_SCALAR_MULSC : XSC_SCALAR_ADDSC;
	*form = *op;
	XscAlu *const f = &form->alu;
	f->scalar_opcode = opcode;
	if (constant)
		f->scalar_constant = a.reg;
	// src3's a is its lane w's component and b its lane x's (xenia:src/xenia/gpu/ucode.h:952-953).
	Source s = constant ? b : a;
	s.negate = a.negate;
	for (int lane = 0; lane < 4; lane++)
		s.components[lane] = lane == 3 ? ca : cb;
	set_source(form, 3, &s);
	f->scalar_dest = f->vector_dest;
	f->scalar_write_mask = mask;
	f->scalar_clamp = f->vector_clamp;
	f->vector_write_mask = 0;
	f->vector_clamp = false;
	return true;
}

// Whether op works in the scalar pipe alone, or, if convert, can, as form does.
static bool scalar_ready(const XscProgram *p, const XscOp *op, bool convert, XscOp *form)
{
	if (scalar_only(op)) {
		*form = *op;
		return true;
	}
	return convert && scalar_form(p, op, form);
}

// Puts x's working pipe into y, which keeps its own, one of them taking the scalar pipe as its scalar form if convert
// where it works in the vector one (optimize.py merge).
static bool merge(const XscProgram *p, XscOp *y, const XscOp *x, bool convert)
{
	XscOp form;
	const bool x_vector = vector_only(x) && scalar_ready(p, y, convert, &form);
	if (!x_vector && !(vector_only(y) && scalar_ready(p, x, convert, &form)))
		return false;
	const XscOp *const vec = x_vector ? x : y;
	const XscAlu *const sca = &form.alu;
	// An export's scalar op writes the vector op's export register (xenia:src/xenia/gpu/ucode.h:2041-2059).
	if (vec->alu.is_export != sca->is_export ||
		(sca->is_export && (sca->vector_dest != vec->alu.vector_dest || (sca->scalar_write_mask & export_lanes(vec)))))
		return false;
	// A scalar op's constant could take the vector op's index (xenia:src/xenia/gpu/ucode.h:1945-1964).
	const bool reads_constant = !sca->src_is_temp[2] || with_constant(sca->scalar_opcode);
	if (addresses_constants(sca) || (reads_constant && addresses_constants(&vec->alu)))
		return false;
	XscAlu merged = vec->alu;
	merged.scalar_opcode = sca->scalar_opcode;
	merged.scalar_dest = sca->scalar_dest;
	merged.scalar_write_mask = sca->scalar_write_mask;
	merged.scalar_clamp = sca->scalar_clamp;
	merged.is_scalar_dest_relative = sca->is_scalar_dest_relative;
	merged.src_reg[2] = sca->src_reg[2];
	merged.src_const_rel[2] = sca->src_const_rel[2];
	merged.src_is_temp[2] = sca->src_is_temp[2];
	merged.src_swizzle[2] = sca->src_swizzle[2];
	merged.src_negate[2] = sca->src_negate[2];
	merged.scalar_constant = sca->scalar_constant;
	const XscAlu saved = y->alu;
	y->alu = merged;
	if (!constants_fit(y)) {
		y->alu = saved;
		return false;
	}
	return true;
}

static size_t stage_of(const XscProgram *p, const XscOp *op)
{
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			if (p->stages[si].ops[i] == op)
				return si;
		}
	}
	return SIZE_MAX;
}

// Where op j's stage starts and ends among the ops flatten lays out, a stage after another.
static void stage_bounds(const XscProgram *p, size_t j, size_t *first, size_t *end)
{
	*first = 0;
	*end = 0;
	for (size_t si = 0; si < p->stage_count && *end <= j; si++) {
		*first = *end;
		*end += p->stages[si].count;
	}
}

// Whether op j joined an op before or after it in its stage (optimize.py co_issue's inner loop, both ways): the nearest
// it can, or one that needn't take its scalar form before one that must.
static bool join(Optimizer *o, size_t j, bool later, bool nearest)
{
	XscOp *const x = o->ops[j];
	KeySet x_reads, x_defs;
	temp_reads(x, &x_reads);
	defs(x, &x_defs);
	size_t first, end;
	stage_bounds(o->p, j, &first, &end);
	const size_t reach = later ? end - 1 - j : j - first;
	XscOp *converting = NULL;
	bool merged = false;
	for (size_t n = 1; n <= reach && !merged; n++) {
		XscOp *const y = o->ops[later ? j + n : j - n];
		KeySet y_defs, y_reads;
		defs(y, &y_defs);
		temp_reads(y, &y_reads);
		// Joined, both pipes read before either writes: the later mustn't read what the earlier writes.
		if (later ? intersects(&y_reads, &x_defs) : intersects(&x_reads, &y_defs))
			break;
		if (!intersects(&x_defs, &y_defs)) {
			merged = merge(o->p, y, x, nearest);
			const XscAlu saved = y->alu;
			if (!merged && !nearest && !converting && merge(o->p, y, x, true)) {
				y->alu = saved;
				converting = y;
			}
		}
		// Moving x past y: neither may read or write what the other writes, nor y change state, nor two exports swap
		// what they write to one lane.
		if (intersects(&x_reads, &y_defs) || intersects(&x_defs, &y_reads) || intersects(&x_defs, &y_defs) ||
			(is_alu(y) && fixed(y)) ||
			((export_lanes(x) & export_lanes(y)) && x->alu.vector_dest == y->alu.vector_dest))
			break;
	}
	if (!merged && !(converting && merge(o->p, converting, x, true)))
		return false;
	XscOp *doomed = x;
	remove_ops(o, &doomed, 1);
	return true;
}

static bool co_issue(Optimizer *o, bool nearest, bool *changed)
{
	*changed = false;
	if (!flatten(o))
		return false;
	// An op joins a later one only where none joins an earlier one, so the pairs found before stay as they were.
	for (int later = 0; later <= 1; later++) {
		for (size_t j = 0; j < o->count;) {
			const XscOp *const x = o->ops[j];
			if (!(vector_only(x) || scalar_only(x)) || !join(o, j, later, nearest)) {
				j++;
				continue;
			}
			// The scan goes on past a pair rather than starting over; the next round takes any join the pair opened.
			*changed = true;
			o->count--;
			memmove(&o->ops[j], &o->ops[j + 1], (o->count - j) * sizeof *o->ops);
		}
	}
	return true;
}

// The ops in order, each one's ALU fields and each stage's count: all co_issue changes.
typedef struct {
	XscOp **order;
	XscAlu *alus;
	size_t *counts;
	size_t count;
} Pairing;

// Keeps what co_issue changes of the ops flatten laid out, to bring it back.
static bool keep(Optimizer *o, Pairing *kept)
{
	kept->count = o->count;
	kept->order = xsdk_xsc_alloc(o->x, (o->count + 1) * sizeof *kept->order);
	kept->alus = xsdk_xsc_alloc(o->x, (o->count + 1) * sizeof *kept->alus);
	kept->counts = xsdk_xsc_alloc(o->x, (o->p->stage_count + 1) * sizeof *kept->counts);
	if (!kept->order || !kept->alus || !kept->counts)
		return false;
	for (size_t i = 0; i < o->count; i++) {
		kept->order[i] = o->ops[i];
		kept->alus[i] = o->ops[i]->alu;
	}
	for (size_t si = 0; si < o->p->stage_count; si++)
		kept->counts[si] = o->p->stages[si].count;
	return true;
}

static void bring_back(Optimizer *o, const Pairing *kept)
{
	size_t at = 0;
	for (size_t si = 0; si < o->p->stage_count; si++) {
		XscStage *const stage = &o->p->stages[si];
		stage->count = kept->counts[si];
		for (size_t i = 0; i < stage->count; i++, at++) {
			stage->ops[i] = kept->order[at];
			stage->ops[i]->alu = kept->alus[at];
		}
	}
	memcpy(o->ops, kept->order, kept->count * sizeof *o->ops);
	o->count = kept->count;
}

// Pairs one way till no pair is left.
static bool pair_all(Optimizer *o, bool nearest)
{
	bool changed = true;
	while (changed) {
		if (!co_issue(o, nearest, &changed))
			return false;
	}
	return flatten(o);
}

// Pairing is greedy, so it runs twice from one start, each op taking its nearest partner, then a scalar op before one
// it converts, and keeps the run with fewer instructions, the second on a tie, which leaves registers as they were
// more.
static bool pair_ops(Optimizer *o)
{
	Pairing start, nearest;
	if (!flatten(o) || !keep(o, &start) || !pair_all(o, true) || !keep(o, &nearest))
		return false;
	bring_back(o, &start);
	if (!pair_all(o, false))
		return false;
	if (nearest.count < o->count)
		bring_back(o, &nearest);
	return true;
}

// Ops whose result is the same in every lane (xenia:src/xenia/gpu/shader_interpreter.cc:436-519, 614-618).
static bool replicates(int64_t op)
{
	return op == XSC_VECTOR_DP4 || op == XSC_VECTOR_DP3 || op == XSC_VECTOR_DP2_ADD || op == XSC_VECTOR_MAX4;
}

// A max of one temp with itself into a temp or an export, clamped or not, alone in its instruction.
static bool is_copy(const XscOp *op)
{
	if (fixed(op) || scalar_active(op) || op->alu.vector_opcode != XSC_VECTOR_MAX || !op->alu.vector_write_mask)
		return false;
	if (op->alu.is_export && (op->alu.scalar_write_mask || op->alu.is_scalar_dest_relative))
		return false;
	const Source a = source(op, 1);
	const Source b = source(op, 2);
	return same_value(&a, &b) && a.temp && !a.absolute && !a.negate;
}

// Whether nothing in the program writes key k, though it writes k's register: the value there is undefined.
static bool undefined(const Optimizer *o, int64_t k)
{
	const int64_t first = k - k % 4;
	if (k < 0 || first + 4 > o->key_count)
		return false;
	return o->write_start[k] == o->write_start[k + 1] && o->write_start[first] != o->write_start[first + 4];
}

// A writer of what the copies from j on copy into one temp, and the lanes it gives, which they read through s; each
// lane's writer, OUTSIDE where none, and the copy that takes it.
typedef struct {
	size_t i;
	size_t j;
	Source s;
	int64_t lanes;
	const int64_t *writers;
	const size_t *at;
} Place;

static int64_t result_reg(const XscOp *w)
{
	return vector_active(w) ? w->alu.vector_dest : w->alu.scalar_dest;
}

static int64_t result_mask(const XscOp *w)
{
	return vector_active(w) ? w->alu.vector_write_mask : w->alu.scalar_write_mask;
}

// The copy's lanes whose value writer w gave.
static int64_t lanes_from(const int64_t writers[4], int64_t mask, int64_t w)
{
	int64_t lanes = 0;
	for (int lane = 0; lane < 4; lane++) {
		if ((mask >> lane & 1) && writers[lane] == w)
			lanes |= (int64_t)1 << lane;
	}
	return lanes;
}

// The copy's lane that will hold component c of the writer's result, or -1.
static int holder(const Optimizer *o, const Place *pl, int c)
{
	const XscOp *const w = o->ops[pl->i];
	// A scalar result fills each lane it writes (xenia:src/xenia/gpu/shader_interpreter.cc:948-955).
	const bool same = !vector_active(w) || replicates(w->alu.vector_opcode);
	for (int lane = 0; lane < 4; lane++) {
		if ((pl->lanes >> lane & 1) && (same || pl->s.components[lane] == c))
			return lane;
	}
	return -1;
}

// Whether a vector op's result fits the copy's lanes: swizzled to them, the same in all, or in its own lanes.
static bool lands(const Optimizer *o, const Place *pl)
{
	const int64_t op = o->ops[pl->i]->alu.vector_opcode;
	if (elementwise(op) || replicates(op))
		return true;
	for (int lane = 0; lane < 4; lane++) {
		if ((pl->lanes >> lane & 1) && pl->s.components[lane] != lane)
			return false;
	}
	return true;
}

// Whether nothing from the writer to each lane's copy reads or writes that lane, nor the writer reads it.
static bool place_free(const Optimizer *o, const Place *pl)
{
	const XscAlu *const copy = &o->ops[pl->j]->alu;
	// An export's writer moves to the copy instead, so nothing between may write what it reads, a0 among it.
	if (copy->is_export) {
		if (addresses_constants(&o->ops[pl->i]->alu) && o->a0_span[pl->i] != o->a0_span[pl->j])
			return false;
		for (size_t k = pl->i + 1; k < pl->j; k++) {
			if (intersects(&o->defs[k], &o->reads[pl->i]))
				return false;
		}
		return true;
	}
	for (int c = 0; c < 4; c++) {
		if (!(pl->lanes >> c & 1))
			continue;
		const int64_t k = key(copy->vector_dest, c);
		if (has_key(&o->reads[pl->i], k))
			return false;
		for (size_t at = pl->i + 1; at < pl->at[c]; at++) {
			if (has_key(&o->reads[at], k) || has_key(&o->defs[at], k))
				return false;
		}
	}
	return true;
}

// The place of writer w, one of the copies' writers or none (no lanes).
static Place place_of(const Place *pl, int64_t w)
{
	Place q = *pl;
	q.i = (size_t)w;
	q.lanes = w == OUTSIDE ? 0 : lanes_from(pl->writers, ALL, w);
	return q;
}

// Whether op k is one of the copies that go.
static bool copied_by(const Place *pl, size_t k)
{
	return k == pl->at[0] || k == pl->at[1] || k == pl->at[2] || k == pl->at[3];
}

// Whether nothing between from and to but skip writes key k.
static bool unwritten(const Optimizer *o, size_t from, size_t to, size_t skip, int64_t k)
{
	for (size_t at = from + 1; at < to; at++) {
		if (at != skip && has_key(&o->defs[at], k))
			return false;
	}
	return true;
}

// Whether operand n of op r reads the writer's result: its first lane read does, as can_redirect leaves no other.
static bool reads_result(const Optimizer *o, const Place *pl, size_t r, int n, const int64_t lanes[4])
{
	const Source s = source(o->ops[r], n);
	const int64_t t = result_reg(o->ops[pl->i]);
	for (int lane = 0; s.temp && s.reg == t && lane < 4; lane++) {
		if (lanes[n] >> lane & 1)
			return flow_at(o, r, key(t, s.components[lane])) == (int64_t)pl->i;
	}
	return false;
}

// Whether op r, another reader of the writer's result, can read it from the copy's place instead.
static bool can_redirect(const Optimizer *o, const Place *pl, size_t r)
{
	const XscOp *const reader = o->ops[r];
	const int64_t t = result_reg(o->ops[pl->i]);
	const int64_t d = o->ops[pl->j]->alu.vector_dest;
	if (fixed(reader) || d == t)
		return false;
	int64_t lanes[4];
	bool named[4];
	reads(reader, lanes, named);
	for (int n = 1; n <= 3; n++) {
		const Source s = source(reader, n);
		if (!named[n] || !s.temp || s.reg != t)
			continue;
		// An operand reads the copies' writers' results alone or none of them.
		int ours = 0, others = 0;
		for (int lane = 0; lane < 4; lane++) {
			if (!(lanes[n] >> lane & 1))
				continue;
			const Place q = place_of(pl, flow_at(o, r, key(t, s.components[lane])));
			if (!q.lanes) {
				others++;
				continue;
			}
			const int h = holder(o, &q, s.components[lane]);
			if (h < 0 || !unwritten(o, q.i, r, pl->at[h], key(d, h)))
				return false;
			ours++;
		}
		if (ours && others)
			return false;
	}
	return true;
}

static void redirect(Optimizer *o, const Place *pl, size_t r)
{
	XscOp *const reader = o->ops[r];
	int64_t lanes[4];
	bool named[4];
	reads(reader, lanes, named);
	for (int n = 1; n <= 3; n++) {
		if (!named[n] || !reads_result(o, pl, r, n, lanes))
			continue;
		const Source old = source(reader, n);
		Source moved = old;
		moved.reg = o->ops[pl->j]->alu.vector_dest;
		// A lane from another of the copies' writers takes its place; a lane not read may hold anything.
		for (int lane = 0; lane < 4; lane++) {
			const bool read = lanes[n] >> lane & 1;
			const Place q = place_of(pl, read ? flow_at(o, r, key(old.reg, old.components[lane])) : (int64_t)pl->i);
			const int h = holder(o, &q, old.components[lane]);
			moved.components[lane] = h < 0 ? lane : h;
		}
		set_source(reader, n, &moved);
	}
	find_keys(o, r);
}

// Whether the writer can write the copy's lanes itself, its other readers finding its result there unclamped.
static bool can_place(Optimizer *o, const Place *pl, size_t *users)
{
	const XscOp *const w = o->ops[pl->i];
	const XscOp *const copy = o->ops[pl->j];
	const bool vector = plain_vector(w);
	if (!vector && !scalar_only(w))
		return false;
	// A vector op exports after the export's alloc, as the lowering's do (xsc_lower.c, xsdk_xsc_emit_construct).
	if (copy->alu.is_export && (!vector || stage_of(o->p, w) != stage_of(o->p, copy)))
		return false;
	if ((vector && !lands(o, pl)) || !place_free(o, pl))
		return false;
	const size_t count = users_of(o, pl->i, result_reg(w), result_mask(w), users);
	for (size_t u = 0; u < count; u++) {
		if (!copied_by(pl, users[u]) &&
			(copy->alu.is_export || copy->alu.vector_clamp || !can_redirect(o, pl, users[u])))
			return false;
	}
	return true;
}

// Moves op w down its stage to just before op copy, so the exports stay where they were.
static void sink(XscProgram *p, XscOp *w, const XscOp *copy)
{
	XscStage *const stage = &p->stages[stage_of(p, w)];
	size_t at = 0;
	while (stage->ops[at] != w)
		at++;
	for (; stage->ops[at + 1] != copy; at++)
		stage->ops[at] = stage->ops[at + 1];
	stage->ops[at] = w;
}

// The writer writes the copy's lanes, clamped as the copy is (xenia:src/xenia/gpu/shader_interpreter.cc:902-909).
static void place(Optimizer *o, const Place *pl, size_t *users)
{
	XscOp *const w = o->ops[pl->i];
	const XscAlu *const c = &o->ops[pl->j]->alu;
	const size_t count = users_of(o, pl->i, result_reg(w), result_mask(w), users);
	for (size_t u = 0; u < count; u++) {
		if (!copied_by(pl, users[u]))
			redirect(o, pl, users[u]);
	}
	XscAlu *const f = &w->alu;
	if (!vector_active(w)) {
		f->scalar_dest = c->vector_dest;
		f->scalar_write_mask = pl->lanes;
		f->scalar_clamp = f->scalar_clamp || c->vector_clamp;
		find_keys(o, pl->i);
		return;
	}
	int table[3];
	const int operands = vector_reads_table(f->vector_opcode, table);
	for (int n = 1; elementwise(f->vector_opcode) && n <= operands; n++) {
		const Source old = source(w, n);
		Source moved = old;
		for (int lane = 0; lane < 4; lane++)
			moved.components[lane] = old.components[pl->s.components[lane]];
		set_source(w, n, &moved);
	}
	f->vector_dest = c->vector_dest;
	f->vector_write_mask = pl->lanes;
	f->is_export = c->is_export;
	f->vector_clamp = f->vector_clamp || c->vector_clamp;
	find_keys(o, pl->i);
	if (c->is_export)
		sink(o->p, w, o->ops[pl->j]);
}

// Each lane's writer, an undefined lane taking a defined one's; false where a lane has none.
static bool copy_writers(const Optimizer *o, size_t j, Source *s, int64_t writers[4])
{
	const int64_t mask = o->ops[j]->alu.vector_write_mask;
	int defined = -1;
	for (int lane = 0; lane < 4; lane++) {
		writers[lane] = (mask >> lane & 1) ? flow_at(o, j, key(s->reg, s->components[lane])) : OUTSIDE;
		if (writers[lane] != OUTSIDE && defined < 0)
			defined = lane;
	}
	for (int lane = 0; lane < 4; lane++) {
		if (!(mask >> lane & 1) || writers[lane] != OUTSIDE)
			continue;
		if (defined < 0 || !undefined(o, key(s->reg, s->components[lane])))
			return false;
		s->components[lane] = s->components[defined];
		writers[lane] = writers[defined];
	}
	return true;
}

static bool among(const int64_t writers[4], int64_t w)
{
	return w != OUTSIDE && (w == writers[0] || w == writers[1] || w == writers[2] || w == writers[3]);
}

// Whether copy k, after the copy at j, copies more of the first writers' lanes into its temp; if so it joins it.
static bool joins(
	const Optimizer *o, size_t j, size_t k, const int64_t first[4], Source *s, int64_t writers[4], size_t at[4])
{
	const XscOp *const copy = o->ops[j];
	const XscOp *const other = o->ops[k];
	if (k <= j || !is_copy(other) || other->alu.is_export || other->alu.vector_dest != copy->alu.vector_dest ||
		other->alu.vector_clamp != copy->alu.vector_clamp)
		return false;
	const Source t = source(other, 1);
	const int64_t mask = other->alu.vector_write_mask;
	if (t.reg != s->reg)
		return false;
	for (int lane = 0; lane < 4; lane++) {
		if ((mask >> lane & 1) &&
			(writers[lane] != OUTSIDE || !among(first, flow_at(o, k, key(t.reg, t.components[lane])))))
			return false;
	}
	for (int lane = 0; lane < 4; lane++) {
		if (!(mask >> lane & 1))
			continue;
		writers[lane] = flow_at(o, k, key(t.reg, t.components[lane]));
		s->components[lane] = t.components[lane];
		at[lane] = k;
	}
	return true;
}

// Whether later copies of the writers' other lanes into the copy's temp join it, to go together.
static bool gather(Optimizer *o, size_t j, Source *s, int64_t writers[4], size_t at[4], size_t *users)
{
	const XscAlu *const copy = &o->ops[j]->alu;
	if (copy->is_export || copy->vector_dest == s->reg)
		return false;
	const int64_t first[4] = { writers[0], writers[1], writers[2], writers[3] };
	bool any = false;
	for (int lane = 0; lane < 4; lane++) {
		const XscOp *const w = first[lane] == OUTSIDE ? NULL : o->ops[first[lane]];
		const size_t count = w ? users_of(o, (size_t)first[lane], result_reg(w), result_mask(w), users) : 0;
		for (size_t u = 0; u < count; u++)
			any = joins(o, j, users[u], first, s, writers, at) || any;
	}
	return any;
}

// Each writer's place in places and how many, or 0 where one can't write its lanes.
static int fit(Optimizer *o, const Place *copies, Place places[4], size_t *users)
{
	int count = 0;
	int64_t seen = 0;
	for (int lane = 0; lane < 4; lane++) {
		const int64_t w = copies->writers[lane];
		if (w == OUTSIDE || (seen >> lane & 1))
			continue;
		places[count] = place_of(copies, w);
		seen |= places[count].lanes;
		if (!can_place(o, &places[count++], users))
			return 0;
	}
	return count;
}

// Whether the copy at j went, with any later copies at[] took, each instruction they copy from now writing its lanes.
static bool try_place(Optimizer *o, size_t j, size_t at[4], size_t *users)
{
	XscOp *const copy = o->ops[j];
	if (!is_copy(copy))
		return false;
	Source s = source(copy, 1);
	int64_t writers[4];
	if (!copy_writers(o, j, &s, writers))
		return false;
	at[0] = at[1] = at[2] = at[3] = j;
	Place copies = { .j = j, .s = s, .writers = writers, .at = at };
	Place places[4];
	int count = fit(o, &copies, places, users);
	if (!count && gather(o, j, &copies.s, writers, at, users))
		count = fit(o, &copies, places, users);
	if (!count)
		return false;
	for (int n = 0; n < count; n++)
		place(o, &places[n], users);
	XscOp *doomed[4];
	for (int lane = 0; lane < 4; lane++)
		doomed[lane] = o->ops[at[lane]];
	remove_ops(o, doomed, 4);
	return true;
}

// Takes op j, a copy that went, out of the flow.
static void drop_op(Optimizer *o, size_t j)
{
	o->count--;
	memmove(&o->ops[j], &o->ops[j + 1], (o->count - j) * sizeof *o->ops);
	memmove(&o->reads[j], &o->reads[j + 1], (o->count - j) * sizeof *o->reads);
	memmove(&o->defs[j], &o->defs[j + 1], (o->count - j) * sizeof *o->defs);
}

// Takes the copies that went, j and any after it at[] names, out of the flow, the last first so the others stay put.
static bool drop_copies(Optimizer *o, size_t j, const size_t at[4])
{
	for (size_t k = o->count; k-- > j;) {
		if (k == at[0] || k == at[1] || k == at[2] || k == at[3])
			drop_op(o, k);
	}
	return index_flow(o);
}

// Takes every copy it can before the round starts over, the scan starting over on the flow as it stands after each.
static bool compute_in_place(Optimizer *o, bool *changed)
{
	*changed = false;
	if (!build_flow(o))
		return false;
	fill_readers(o);
	size_t *const users = xsdk_xsc_alloc(o->x, (4 * o->count + 1) * sizeof *users);
	if (!users)
		return false;
	size_t at[4];
	size_t j = 0;
	while (j < o->count) {
		if (!try_place(o, j, at, users)) {
			j++;
			continue;
		}
		*changed = true;
		// An export's writer moved down to it, so the program is read again.
		if (!(o->ops[j]->alu.is_export ? build_flow(o) : drop_copies(o, j, at)))
			return false;
		fill_readers(o);
		j = 0;
	}
	return true;
}

// Whether the passes model how an op reads constants: at a0 alone, through the operands' own flags, and no a0 load
// reading one at a0.
static bool a0_modelled(const XscAlu *f)
{
	if (f->is_const_0_addressed || f->is_const_1_addressed)
		return false;
	for (int n = 0; n < 3; n++) {
		if (f->src_const_rel[n] && !f->src_is_temp[n] && (!f->is_const_address_register_relative || loads_a0(f)))
			return false;
	}
	return true;
}

// Programs the passes leave alone (optimize.py can_optimize); unmodelled if one reads a constant at a run-time index
// in a way the passes don't model, which co_issue alone takes.
static bool can_optimize(const XscProgram *p, bool *unmodelled)
{
	*unmodelled = false;
	if (!p->whole_sources || p->loop_count)
		return false;
	for (size_t si = 0; si < p->stage_count; si++) {
		for (size_t i = 0; i < p->stages[si].count; i++) {
			const XscOp *const op = p->stages[si].ops[i];
			if (!is_alu(op)) {
				if (op->fetch.is_src_relative || op->fetch.is_dest_relative || op->fetch.is_predicated)
					return false;
				continue;
			}
			const XscAlu *const f = &op->alu;
			if (f->is_vector_dest_relative || f->is_predicated || f->abs_constants)
				return false;
			*unmodelled = *unmodelled || !a0_modelled(f);
			int64_t scalar_mask = f->scalar_write_mask;
			if (f->is_export)
				scalar_mask &= ~f->vector_write_mask;
			if (scalar_previous(f->scalar_opcode) || (f->scalar_opcode == XSC_SCALAR_RETAIN_PREV && scalar_mask))
				return false;
			if (f->is_scalar_dest_relative && !f->is_export)
				return false;
		}
	}
	return true;
}

static bool first_round(Optimizer *o, bool *changed)
{
	if (!remove_dead(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!common_subexpressions(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!propagate_copies(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!fuse_multiply_adds(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	if (!fold_by_ranges(o, changed) || *changed)
		return o->x->status == XSDK_XSC_OK;
	return compute_in_place(o, changed);
}

bool xsdk_xsc_optimize(Xsc *x, XscProgram *p, bool *ran)
{
	bool unmodelled;
	*ran = false;
	if (!can_optimize(p, &unmodelled))
		return true;
	Optimizer o = { .x = x, .p = p, .key_count = (p->max_reg + 2) * 4 };
	o.live = xsdk_xsc_alloc(x, (size_t)o.key_count + 1);
	if (!o.live)
		return false;
	// The first round moves a read at a0 only where it reads the same a0 (same_values); co_issue moves nothing past
	// the op that sets a0, nor past an alloc.
	bool changed = !unmodelled;
	while (changed) {
		if (!first_round(&o, &changed))
			return false;
	}
	if (!pair_ops(&o))
		return false;
	*ran = !unmodelled;
	return unmodelled || remove_dead(&o, &changed);
}

// ---- what the allocator needs to pack values into components (xsc_program.c) ----

size_t xsdk_xsc_read_keys(const XscOp *op, int64_t keys[XSC_MAX_KEYS])
{
	KeySet set;
	temp_reads(op, &set);
	memcpy(keys, set.keys, set.count * sizeof *keys);
	return set.count;
}

size_t xsdk_xsc_write_keys(const XscOp *op, int64_t keys[XSC_MAX_KEYS])
{
	KeySet set;
	defs(op, &set);
	memcpy(keys, set.keys, set.count * sizeof *keys);
	return set.count;
}

bool xsdk_xsc_lanes_fixed(const XscOp *op)
{
	if (!is_alu(op) || op->alu.is_export || !op->alu.vector_write_mask || replicates(op->alu.vector_opcode))
		return false;
	// An elementwise op's operands move with its lanes, but a scalar op beside it reads operand 3 in its own.
	int table[3];
	return !elementwise(op->alu.vector_opcode) ||
		(scalar_active(op) && vector_reads_table(op->alu.vector_opcode, table) == 3);
}

static int64_t moved_mask(int64_t mask, const uint8_t lanes[4])
{
	int64_t moved = 0;
	for (int c = 0; c < 4; c++) {
		if (mask >> c & 1)
			moved |= (int64_t)1 << lanes[c];
	}
	return moved;
}

void xsdk_xsc_move_lanes(XscOp *op, const uint8_t (*lanes)[4], int64_t count)
{
	if (!is_alu(op))
		return;
	XscAlu *const f = &op->alu;
	static const uint8_t same[4] = { 0, 1, 2, 3 };
	const bool vector_temp = !f->is_export && f->vector_write_mask && f->vector_dest < count;
	const uint8_t *const to = vector_temp ? lanes[f->vector_dest] : same;
	int table[3];
	const int operands = elementwise(f->vector_opcode) ? vector_reads_table(f->vector_opcode, table) : 0;
	for (int n = 1; n <= 3; n++) {
		const Source s = source(op, n);
		Source moved = s;
		// An elementwise result's lane takes its operands' same lane.
		for (int lane = 0; n <= operands && lane < 4; lane++)
			moved.components[to[lane]] = s.components[lane];
		// mulsc's src3 reads its temp in lane x alone; lane w names its constant's component.
		const int temp_lanes = n == 3 && with_constant(f->scalar_opcode) ? 1 : 4;
		for (int lane = 0; moved.temp && moved.reg < count && lane < temp_lanes; lane++)
			moved.components[lane] = lanes[moved.reg][moved.components[lane]];
		set_source(op, n, &moved);
	}
	if (vector_temp)
		f->vector_write_mask = moved_mask(f->vector_write_mask, to);
	if (!f->is_export && f->scalar_write_mask && f->scalar_dest < count)
		f->scalar_write_mask = moved_mask(f->scalar_write_mask, lanes[f->scalar_dest]);
}
