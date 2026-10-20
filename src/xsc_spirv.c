#include <stdio.h>
#include <string.h>

#include "xsc_private.h"

// The SPIR-V reader, as xenos-toolchain/xsc/spirv.py reads a module.

enum {
	HEADER_WORDS = 5,
	ENTRY_POINT_NAME_WORD = 2,
};

static const struct {
	uint32_t opcode;
	const char *name;
} op_names[] = { { 0, "Nop" }, { 3, "Source" }, { 5, "Name" }, { 6, "MemberName" }, { 11, "ExtInstImport" },
	{ 12, "ExtInst" }, { 14, "MemoryModel" }, { 15, "EntryPoint" }, { 16, "ExecutionMode" }, { 17, "Capability" },
	{ 19, "TypeVoid" }, { 20, "TypeBool" }, { 21, "TypeInt" }, { 22, "TypeFloat" }, { 23, "TypeVector" },
	{ 24, "TypeMatrix" }, { 25, "TypeImage" }, { 26, "TypeSampler" }, { 27, "TypeSampledImage" }, { 28, "TypeArray" },
	{ 30, "TypeStruct" }, { 32, "TypePointer" }, { 33, "TypeFunction" }, { 43, "Constant" },
	{ 44, "ConstantComposite" }, { 54, "Function" }, { 55, "FunctionParameter" }, { 56, "FunctionEnd" },
	{ 57, "FunctionCall" }, { 59, "Variable" }, { 61, "Load" }, { 62, "Store" }, { 65, "AccessChain" },
	{ 71, "Decorate" }, { 72, "MemberDecorate" }, { 79, "VectorShuffle" }, { 80, "CompositeConstruct" },
	{ 81, "CompositeExtract" }, { 82, "CompositeInsert" }, { 84, "Transpose" }, { 87, "ImageSampleImplicitLod" },
	{ 88, "ImageSampleExplicitLod" }, { 89, "ImageSampleDrefImplicitLod" }, { 90, "ImageSampleDrefExplicitLod" },
	{ 91, "ImageSampleProjImplicitLod" }, { 92, "ImageSampleProjExplicitLod" }, { 110, "ConvertFToS" },
	{ 111, "ConvertSToF" }, { 124, "Bitcast" }, { 127, "FNegate" }, { 128, "IAdd" }, { 129, "FAdd" }, { 130, "ISub" },
	{ 131, "FSub" }, { 132, "IMul" }, { 133, "FMul" }, { 134, "UDiv" }, { 135, "SDiv" }, { 136, "FDiv" },
	{ 137, "UMod" }, { 138, "SRem" }, { 139, "SMod" }, { 142, "VectorTimesScalar" }, { 144, "VectorTimesMatrix" },
	{ 145, "MatrixTimesVector" }, { 146, "MatrixTimesMatrix" }, { 148, "Dot" }, { 154, "Any" }, { 155, "All" },
	{ 166, "LogicalOr" }, { 167, "LogicalAnd" }, { 168, "LogicalNot" }, { 169, "Select" }, { 170, "IEqual" },
	{ 171, "INotEqual" }, { 172, "UGreaterThan" }, { 173, "SGreaterThan" }, { 174, "UGreaterThanEqual" },
	{ 175, "SGreaterThanEqual" }, { 176, "ULessThan" }, { 177, "SLessThan" }, { 178, "ULessThanEqual" },
	{ 179, "SLessThanEqual" }, { 180, "FOrdEqual" }, { 182, "FOrdNotEqual" }, { 184, "FOrdLessThan" },
	{ 186, "FOrdGreaterThan" }, { 188, "FOrdLessThanEqual" }, { 190, "FOrdGreaterThanEqual" },
	{ 194, "ShiftRightLogical" }, { 195, "ShiftRightArithmetic" }, { 196, "ShiftLeftLogical" }, { 197, "BitwiseOr" },
	{ 198, "BitwiseXor" }, { 199, "BitwiseAnd" }, { 200, "Not" }, { 207, "DPdx" }, { 208, "DPdy" }, { 209, "Fwidth" },
	{ 246, "LoopMerge" }, { 247, "SelectionMerge" }, { 248, "Label" }, { 249, "Branch" }, { 250, "BranchConditional" },
	{ 252, "Kill" }, { 253, "Return" }, { 254, "ReturnValue" }, { 41, "ConstantTrue" }, { 42, "ConstantFalse" },
	{ 245, "Phi" } };

// Opcodes whose first two words are a result type and id, and those with an id alone (spirv.py).
static const uint16_t type_and_result[] = { 12, 43, 44, 54, 55, 57, 59, 61, 65, 79, 80, 81, 82, 84, 87, 88, 89, 90, 91,
	92, 124, 127, 129, 130, 131, 133, 136, 141, 142, 144, 145, 146, 148, 169, 180, 182, 184, 186, 188, 190, 110, 111,
	128, 132, 134, 135, 137, 138, 139, 154, 155, 166, 167, 168, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 194,
	195, 196, 197, 198, 199, 200, 207, 208, 209, 41, 42, 245, 1, 46 };
static const uint16_t result_only[] = { 11, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 32, 33, 248 };

static bool listed(const uint16_t *list, size_t count, uint32_t opcode)
{
	for (size_t i = 0; i < count; i++) {
		if (list[i] == opcode)
			return true;
	}
	return false;
}

static uint32_t skipped_words(uint32_t opcode)
{
	if (opcode >= XSC_OP_LOOP_PHI)
		return 2;
	if (listed(type_and_result, sizeof type_and_result / sizeof type_and_result[0], opcode))
		return 2;
	if (listed(result_only, sizeof result_only / sizeof result_only[0], opcode))
		return 1;
	return 0;
}

const uint32_t *xsdk_xsc_operands(const XscInstr *ins, uint32_t *count)
{
	uint32_t skip = skipped_words(ins->opcode);
	if (skip > ins->word_count)
		skip = ins->word_count;
	*count = ins->word_count - skip;
	return ins->words + skip;
}

uint32_t xsdk_xsc_operand_count(const XscInstr *ins)
{
	uint32_t count;
	(void)xsdk_xsc_operands(ins, &count);
	return count;
}

uint32_t xsdk_xsc_operand(const XscInstr *ins, uint32_t i)
{
	uint32_t count;
	const uint32_t *const operands = xsdk_xsc_operands(ins, &count);
	return i < count ? operands[i] : 0;
}

const char *xsdk_xsc_op_name(uint32_t opcode, char *buffer, size_t buffer_bytes)
{
	for (size_t i = 0; i < sizeof op_names / sizeof op_names[0]; i++) {
		if (op_names[i].opcode == opcode)
			return op_names[i].name;
	}
	(void)snprintf(buffer, buffer_bytes, "Op%u", (unsigned)opcode);
	return buffer;
}

// A NUL-ended string from words, low byte first; its words, the NUL's included, through *used.
static const char *decode_string(Xsc *x, const uint32_t *words, uint32_t count, uint32_t *used)
{
	char *const text = xsdk_xsc_alloc(x, (size_t)count * 4 + 1);
	if (!text)
		return NULL;
	size_t length = 0;
	uint32_t i = 0;
	bool ended = false;
	while (i < count && !ended) {
		for (int shift = 0; shift < 32; shift += 8) {
			const char byte = (char)(words[i] >> shift & 0xFF);
			if (!byte) {
				ended = true;
				break;
			}
			text[length++] = byte;
		}
		i++;
	}
	*used = i;
	return text;
}

static bool add_name(Xsc *x, XscName **names, size_t *count, size_t *capacity, XscName name)
{
	if (!xsdk_xsc_room(x, names, *count, capacity, sizeof **names))
		return false;
	(*names)[(*count)++] = name;
	return true;
}

static bool add_decoration(Xsc *x, XscDecoration **list, size_t *count, size_t *capacity, XscDecoration decoration)
{
	if (!xsdk_xsc_room(x, list, *count, capacity, sizeof **list))
		return false;
	(*list)[(*count)++] = decoration;
	return true;
}

typedef struct {
	size_t names;
	size_t member_names;
	size_t decorations;
	size_t member_decorations;
	size_t entry_points;
} Capacities;

static bool index_one(Xsc *x, XscModule *m, const XscInstr *ins, Capacities *capacity)
{
	const uint32_t *const w = ins->words;
	const uint32_t n = ins->word_count;
	uint32_t used;
	switch (ins->opcode) {
	case SPV_NAME:
		if (n < 1)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "OpName without a target");
		return add_name(x, &m->names, &m->name_count, &capacity->names,
			(XscName){ .target = w[0], .name = decode_string(x, w + 1, n - 1, &used) });
	case SPV_MEMBER_NAME:
		if (n < 2)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "OpMemberName without a member");
		return add_name(x, &m->member_names, &m->member_name_count, &capacity->member_names,
			(XscName){ .target = w[0], .member = w[1], .name = decode_string(x, w + 2, n - 2, &used) });
	case SPV_DECORATE:
	// A string's words are its arguments, as an HLSL semantic's are (xsc_resolve.c).
	case SPV_DECORATE_STRING:
		if (n < 2)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "OpDecorate without a decoration");
		return add_decoration(x, &m->decorations, &m->decoration_count, &capacity->decorations,
			(XscDecoration){ .target = w[0], .decoration = w[1], .args = w + 2, .arg_count = n - 2 });
	case SPV_MEMBER_DECORATE:
	// An HLSL member's register(bN) or register(iN), as glslang-xenon keeps it (xsc_resolve.c).
	case SPV_MEMBER_DECORATE_STRING:
		if (n < 3)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "OpMemberDecorate without a decoration");
		return add_decoration(x, &m->member_decorations, &m->member_decoration_count, &capacity->member_decorations,
			(XscDecoration){ .target = w[0], .member = w[1], .decoration = w[2], .args = w + 3, .arg_count = n - 3 });
	case SPV_ENTRY_POINT: {
		if (n < ENTRY_POINT_NAME_WORD)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "OpEntryPoint without an id");
		const char *const name = decode_string(x, w + ENTRY_POINT_NAME_WORD, n - ENTRY_POINT_NAME_WORD, &used);
		if (!xsdk_xsc_room(x, &m->entry_points, m->entry_point_count, &capacity->entry_points, sizeof *m->entry_points))
			return false;
		m->entry_points[m->entry_point_count++] = (XscEntryPoint){ .model = w[0],
			.id = w[1],
			.name = name,
			.interface = w + ENTRY_POINT_NAME_WORD + used,
			.interface_count = n - ENTRY_POINT_NAME_WORD - used };
		return name != NULL;
	}
	default:
		return true;
	}
}

bool xsdk_xsc_spirv_index(Xsc *x, XscModule *m)
{
	m->name_count = m->member_name_count = m->decoration_count = m->member_decoration_count = 0;
	m->entry_point_count = 0;
	m->names = m->member_names = NULL;
	m->decorations = m->member_decorations = NULL;
	m->entry_points = NULL;
	uint32_t limit = m->bound;
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].result_id >= limit)
			limit = m->instrs[i].result_id + 1;
	}
	m->id_limit = limit;
	m->by_result = xsdk_xsc_alloc(x, (size_t)limit * sizeof *m->by_result);
	if (!m->by_result)
		return false;
	for (uint32_t id = 0; id < limit; id++)
		m->by_result[id] = -1;
	Capacities capacity = { 0 };
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->result_id)
			m->by_result[ins->result_id] = (int32_t)i;
		if (!index_one(x, m, ins, &capacity))
			return false;
	}
	return true;
}

bool xsdk_xsc_spirv_parse(Xsc *x, const uint32_t *words, size_t word_count, XscModule *m)
{
	memset(m, 0, sizeof *m);
	if (word_count < HEADER_WORDS)
		return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "too short for a SPIR-V header");
	uint32_t *const copy = xsdk_xsc_alloc(x, word_count * sizeof *copy);
	if (!copy)
		return false;
	memcpy(copy, words, word_count * sizeof *copy);
	if (copy[0] != SPV_MAGIC) {
		// A module of the other byte order is swapped.
		if (__builtin_bswap32(copy[0]) != SPV_MAGIC)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "bad SPIR-V magic 0x%08x", (unsigned)copy[0]);
		for (size_t i = 0; i < word_count; i++)
			copy[i] = __builtin_bswap32(copy[i]);
	}
	m->version = copy[1];
	m->generator = copy[2];
	m->bound = copy[3];
	size_t capacity = 0;
	size_t i = HEADER_WORDS;
	while (i < word_count) {
		const uint32_t count = copy[i] >> 16;
		const uint32_t opcode = copy[i] & 0xFFFF;
		if (!count)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "zero word-count at %zu", i);
		const uint32_t available = (uint32_t)(word_count - i - 1 < count - 1 ? word_count - i - 1 : count - 1);
		XscInstr ins = { .opcode = opcode, .words = copy + i + 1, .word_count = available };
		const uint32_t skip = skipped_words(opcode);
		if (skip > available)
			return xsdk_xsc_fail(x, XSDK_XSC_MALFORMED, "Op%u too short for its result", (unsigned)opcode);
		if (skip == 2) {
			ins.result_type = ins.words[0];
			ins.result_id = ins.words[1];
		} else if (skip == 1) {
			ins.result_id = ins.words[0];
		}
		if (!xsdk_xsc_room(x, &m->instrs, m->instr_count, &capacity, sizeof *m->instrs))
			return false;
		m->instrs[m->instr_count++] = ins;
		i += count;
	}
	return xsdk_xsc_spirv_index(x, m);
}

// The opcode of a 32-bit scalar type's zero, or 0 for another type.
static uint32_t zero_opcode(const XscInstr *type)
{
	if (type && type->opcode == SPV_TYPE_BOOL)
		return SPV_CONSTANT_FALSE;
	const bool number = type && (type->opcode == SPV_TYPE_FLOAT || type->opcode == SPV_TYPE_INT);
	return number && xsdk_xsc_operand(type, 0) == 32 ? SPV_CONSTANT : 0;
}

// A constant of type and id: a zero, or the lane zeros of a vector, lane_count of them.
static bool make_constant(
	Xsc *x, XscInstr *ins, uint32_t opcode, uint32_t type, uint32_t id, uint32_t lane_zero, uint32_t lane_count)
{
	const uint32_t count = opcode == SPV_CONSTANT_COMPOSITE ? 2 + lane_count : opcode == SPV_CONSTANT ? 3 : 2;
	uint32_t *const words = xsdk_xsc_alloc(x, count * sizeof *words);
	if (!words)
		return false;
	words[0] = type;
	words[1] = id;
	for (uint32_t i = 2; opcode == SPV_CONSTANT_COMPOSITE && i < count; i++)
		words[i] = lane_zero;
	*ins = (XscInstr){ opcode, type, id, words, count };
	return true;
}

// An undefined value may be any (SPIR-V 1.0, section 3.32.1, OpUndef), and a null one is 0, +0.0 or false in each
// component (section 3.32.7, OpConstantNull). A vector's lanes take a zero minted before the first vector that needs
// it.
bool xsdk_xsc_spirv_zero_nulls(Xsc *x, XscModule *m)
{
	size_t head = 0;
	while (head < m->instr_count && m->instrs[head].opcode != SPV_FUNCTION)
		head++;
	uint32_t *const lane_zero = xsdk_xsc_alloc(x, (size_t)m->id_limit * sizeof *lane_zero);
	XscInstr *const minted = xsdk_xsc_alloc(x, (head + 1) * sizeof *minted);
	if (!lane_zero || !minted)
		return false;
	size_t minted_count = 0;
	size_t at = head;
	for (size_t i = 0; i < head; i++) {
		XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_UNDEF && ins->opcode != SPV_CONSTANT_NULL)
			continue;
		const XscInstr *const type = xsdk_xsc_by_result(m, ins->result_type);
		const uint32_t opcode = zero_opcode(type);
		if (opcode) {
			if (!make_constant(x, ins, opcode, ins->result_type, ins->result_id, 0, 0))
				return false;
			continue;
		}
		if (!type || type->opcode != SPV_TYPE_VECTOR)
			continue;
		const uint32_t comp = xsdk_xsc_operand(type, 0);
		const uint32_t lanes = xsdk_xsc_operand(type, 1);
		const uint32_t lane_opcode = zero_opcode(xsdk_xsc_by_result(m, comp));
		if (!lane_opcode || lanes < 2 || lanes > 4)
			continue;
		if (!lane_zero[comp]) {
			lane_zero[comp] = m->id_limit + (uint32_t)minted_count;
			if (!make_constant(x, &minted[minted_count++], lane_opcode, comp, lane_zero[comp], 0, 0))
				return false;
			at = at < i ? at : i;
		}
		if (!make_constant(x, ins, SPV_CONSTANT_COMPOSITE, ins->result_type, ins->result_id, lane_zero[comp], lanes))
			return false;
	}
	if (!minted_count)
		return true;
	XscInstr *const instrs = xsdk_xsc_alloc(x, (m->instr_count + minted_count) * sizeof *instrs);
	if (!instrs)
		return false;
	memcpy(instrs, m->instrs, at * sizeof *instrs);
	memcpy(instrs + at, minted, minted_count * sizeof *instrs);
	memcpy(instrs + at + minted_count, m->instrs + at, (m->instr_count - at) * sizeof *instrs);
	m->instrs = instrs;
	m->instr_count += minted_count;
	m->bound = m->id_limit + (uint32_t)minted_count;
	return xsdk_xsc_spirv_index(x, m);
}

const XscInstr *xsdk_xsc_by_result(const XscModule *m, uint32_t id)
{
	if (id >= m->id_limit || m->by_result[id] < 0)
		return NULL;
	return &m->instrs[m->by_result[id]];
}

// HLSL's static globals are Private variables, which each invocation holds its own of as it does a Function variable
// (SPIR-V 1.0, section 3.7, Storage Class).
bool xsdk_xsc_declares_local(const XscModule *m, size_t i, size_t first, size_t end)
{
	const XscInstr *const ins = &m->instrs[i];
	if (ins->opcode != SPV_VARIABLE)
		return false;
	const uint32_t storage = xsdk_xsc_operand(ins, 0);
	return storage == SPV_STORAGE_PRIVATE || (storage == SPV_STORAGE_FUNCTION && i >= first && i < end);
}

enum {
	OP_SOURCE = 3,
	SOURCE_HLSL = 5,
};

// OpSource's language (SPIR-V 1.0, section 3.2).
bool xsdk_xsc_hlsl(const XscModule *m)
{
	for (size_t i = 0; i < m->instr_count && m->instrs[i].opcode != SPV_FUNCTION; i++) {
		if (m->instrs[i].opcode == OP_SOURCE)
			return xsdk_xsc_operand(&m->instrs[i], 0) == SOURCE_HLSL;
	}
	return false;
}

const char *xsdk_xsc_name(const XscModule *m, uint32_t id, const char *fallback)
{
	const char *name = fallback;
	for (size_t i = 0; i < m->name_count; i++) {
		if (m->names[i].target == id)
			name = m->names[i].name;
	}
	return name;
}

const char *xsdk_xsc_member_name(const XscModule *m, uint32_t target, uint32_t member)
{
	const char *name = NULL;
	for (size_t i = 0; i < m->member_name_count; i++) {
		if (m->member_names[i].target == target && m->member_names[i].member == member)
			name = m->member_names[i].name;
	}
	return name;
}

const XscDecoration *xsdk_xsc_decoration(const XscModule *m, uint32_t id, uint32_t decoration)
{
	const XscDecoration *found = NULL;
	for (size_t i = 0; i < m->decoration_count; i++) {
		if (m->decorations[i].target == id && m->decorations[i].decoration == decoration)
			found = &m->decorations[i];
	}
	return found;
}

const XscDecoration *xsdk_xsc_member_decoration(
	const XscModule *m, uint32_t target, uint32_t member, uint32_t decoration)
{
	const XscDecoration *found = NULL;
	for (size_t i = 0; i < m->member_decoration_count; i++) {
		const XscDecoration *const d = &m->member_decorations[i];
		if (d->target == target && d->member == member && d->decoration == decoration)
			found = d;
	}
	return found;
}

int64_t xsdk_xsc_decoration_arg(const XscDecoration *d, int64_t fallback)
{
	return d && d->arg_count ? d->args[0] : fallback;
}
