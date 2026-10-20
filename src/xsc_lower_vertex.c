#include "xsc_lower.h"

// A vertex shader, lowered as lower.py's lower_vertex lowers it.

enum {
	// A texture fetch names one of 32 fetch constants (xenia:src/xenia/gpu/ucode.h:789-790).
	FETCH_SLOTS = 32,
	// gl_PerVertex's members by their BuiltIn decoration, and outputs' storage class (SPIR-V 1.0's enums).
	BUILTIN_POINT_SIZE = 1,
	BUILTIN_CLIP_DISTANCE = 3,
	BUILTIN_CULL_DISTANCE = 4,
	STORAGE_OUTPUT = 3,
	// Xenos clips against six planes (xenia:src/xenia/gpu/registers.h:539-544).
	CLIP_PLANES = 6,
};

// Where gl_PerVertex's members are, XSC_NONE for those the block lacks or a gl_Position of its own.
typedef struct {
	int64_t position;
	int64_t point_size;
	int64_t clip_distance;
	int64_t cull_distance;
} PerVertex;

// The unroller merges a vertex shader's if/else arms (xsc_unroll.c); a branch it left would lower as its last arm
// alone, silently.
static bool has_branches(const XscModule *m)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		if (m->instrs[i].opcode == SPV_BRANCH_CONDITIONAL || m->instrs[i].opcode == SPV_SWITCH)
			return true;
	}
	return false;
}

// Each input fetched at index.x into the registers from first: at r0 from r0, r0 last as it's the vertex index
// (xenia:src/xenia/gpu/dxbc_shader_translator_fetch.cc:80-88), or at an instanced shader's t from r1.
static bool fetch_inputs(XscLower *l, const XscInterface *si, int64_t index, int64_t first)
{
	for (size_t n = si->vfetch_count; n-- > 0;) {
		const XscVarUse *const use = &si->vfetch[n];
		XscFetch f = xsdk_xsc_template_vfetch();
		f.dest = first + (int64_t)n;
		f.src = index;
		if (!xsdk_xsc_program_vfetch(l->x, l->p, use->a, use->b, &f))
			return false;
		l->inputs[use->var->vid] = f.dest;
	}
	return true;
}

// k = floor(r0.x * c.x + c.y), t.x = r0.x - c.z * k, t.y = k + c.w, from the instance group c, in an exec of ALU alone
// before the fetches, as the console drew it (docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1");
// gl_InstanceIndex is t.y, held whole in l->instance if the shader reads it. Gives t.
static int64_t prologue(XscLower *l, int64_t group, bool reads_index)
{
	const XscOperand c = xsc_const(group);
	const XscSrc index = xsc_src(xsc_reg(0), false, xsdk_xsc_bsw(0));
	const int64_t q = xsdk_xsc_fresh(l), k = xsdk_xsc_fresh(l), t = xsdk_xsc_fresh(l);
	if (!xsc_emit3(l, XSC_VECTOR_MAD, index, xsc_src(c, false, xsdk_xsc_bsw(0)), xsc_src(c, false, xsdk_xsc_bsw(1)), q,
			false, 0xF) ||
		!xsc_emit2(l, XSC_VECTOR_FLOOR, xsc_temp(q), xsc_temp(q), k, false, 0xF) ||
		!xsc_emit3(l, XSC_VECTOR_MAD, xsc_src(c, true, xsdk_xsc_bsw(2)), xsc_temp(k), index, t, false, 0x1) ||
		!xsc_emit2(l, XSC_VECTOR_ADD, xsc_temp(k), xsc_src(c, false, xsdk_xsc_bsw(3)), t, false, 0x2))
		return XSC_NONE;
	if (reads_index) {
		l->instance = xsdk_xsc_emit_copy(l, xsc_reg(t), XSC_NONE, false, false, xsdk_xsc_bsw(1), 0xF, false);
		if (l->instance == XSC_NONE)
			return XSC_NONE;
	}
	return xsdk_xsc_program_predicate(l->x, l->p, XSC_PREDICATE_NONE) ? t : XSC_NONE;
}

// The member of gl_PerVertex decorated with this BuiltIn.
static int64_t builtin_member(const XscModule *m, uint32_t block_type, int64_t builtin)
{
	for (size_t i = 0; i < m->member_decoration_count; i++) {
		const XscDecoration *const d = &m->member_decorations[i];
		if (d->target == block_type && d->decoration == SPV_DEC_BUILTIN && d->arg_count && d->args[0] == builtin)
			return d->member;
	}
	return XSC_NONE;
}

static PerVertex per_vertex(const XscModule *m, const XscType *types, const XscInterface *si)
{
	PerVertex pv = { xsdk_xsc_role(si, si->position_out)->a, XSC_NONE, XSC_NONE, XSC_NONE };
	const XscInstr *const var = xsdk_xsc_by_result(m, si->position_out);
	const XscType *const pointer = var ? xsdk_xsc_type(m, types, var->result_type) : NULL;
	if (pv.position == XSC_NONE || !pointer)
		return pv;
	pv.point_size = builtin_member(m, pointer->comp, BUILTIN_POINT_SIZE);
	pv.clip_distance = builtin_member(m, pointer->comp, BUILTIN_CLIP_DISTANCE);
	pv.cull_distance = builtin_member(m, pointer->comp, BUILTIN_CULL_DISTANCE);
	return pv;
}

// The gl_PerVertex member a pointer reaches, XSC_NONE for any other pointer.
static int64_t member_of(XscLower *l, uint32_t pointer)
{
	const XscInstr *const chain = xsdk_xsc_by_result(l->m, pointer);
	int64_t member;
	if (!chain || chain->opcode != SPV_ACCESS_CHAIN || xsdk_xsc_operand(chain, 0) != l->si->position_out ||
		!xsdk_xsc_int_const(l, xsdk_xsc_operand(chain, 1), &member))
		return XSC_NONE;
	return member;
}

// Whether a pointer is gl_Position's: a gl_PerVertex's member, or a gl_Position of its own.
static bool is_position_pointer(XscLower *l, const PerVertex *pv, uint32_t pointer)
{
	return pv->position == XSC_NONE ? pointer == l->si->position_out : member_of(l, pointer) == pv->position;
}

// gl_Position's value: its member's store alone, as the block's other members are stored through it too.
static uint32_t position_value(XscLower *l, const PerVertex *pv, const XscStores *stores)
{
	uint32_t value = 0;
	for (size_t i = 0; i < stores->target_count; i++) {
		if (is_position_pointer(l, pv, stores->targets[i]))
			value = stores->value[stores->targets[i]];
	}
	return value;
}

// gl_PointSize to oPts.x, the point's full width in pixels (xenia:src/xenia/gpu/ucode.h:1813-1834), and the record's
// export mode to the sprite's; its edge and kill flags are left unwritten. Without it, points are PA_SU_POINT_SIZE's.
static bool export_point_size(XscLower *l, const PerVertex *pv, const XscStores *stores)
{
	// HLSL's PSIZE is a variable of its own (xsc_resolve.c).
	uint32_t value = l->si->point_size ? stores->value[l->si->point_size->vid] : 0;
	for (size_t i = 0; i < stores->target_count && pv->point_size != XSC_NONE; i++) {
		if (member_of(l, stores->targets[i]) == pv->point_size)
			value = stores->value[stores->targets[i]];
	}
	if (!value)
		return !xsc_failed(l);
	const XscOrigin origin = xsdk_xsc_scalar_origin(l, value);
	if (xsc_failed(l))
		return false;
	const int64_t swizzle = origin.comp == XSC_BROADCAST ? 0 : xsdk_xsc_bsw(origin.comp);
	l->p->exports_point_size = true;
	return xsdk_xsc_emit_copy(l, origin.o, XSC_EXPORT_POINT_SIZE, true, false, swizzle, 0x1, false) != XSC_NONE;
}

static uint32_t stored_value(const XscLower *l, uint32_t ssa);

// A matrix output's column, an interpolator each from its first (xsc_resolve.c, add_varying).
static XscOperand column(XscLower *l, const XscInterface *si, size_t use, uint32_t value)
{
	const XscMatrix *const mat = xsdk_xsc_matrix_operands(l, value);
	size_t c = 0;
	for (size_t i = 0; i < use; i++)
		c += si->interp_out[i].var == si->interp_out[use].var;
	if (mat && (!l->col_major[stored_value(l, value)] || c >= mat->count))
		xsdk_xsc_lower_refuse(l, "interpolator %s, a matrix held by rows (v1)", si->interp_out[use].var->name);
	return mat && !xsc_failed(l) ? mat->rows[c] : (XscOperand){ 0 };
}

static bool export_interpolators(XscLower *l, const XscInterface *si, const XscStores *stores)
{
	for (size_t i = 0; i < si->interp_out_count; i++) {
		const XscVarUse *const use = &si->interp_out[i];
		const uint32_t value = stores->value[use->var->vid];
		if (!value)
			return xsdk_xsc_lower_refuse(l, "interpolator %s never written", use->var->name);
		const XscType *const t = xsdk_xsc_result_type(l, value);
		const XscOperand r =
			t && t->kind == XSC_TYPE_MATRIX ? column(l, si, i, value) : xsdk_xsc_lower_operand(l, value);
		if (xsc_failed(l))
			return false;
		if (r.kind == XSC_OPERAND_MAT)
			return xsdk_xsc_fail(l->x, XSDK_XSC_MALFORMED, "interpolator %s holds a matrix", use->var->name);
		XscAlu f = xsdk_xsc_template_mov();
		for (int n = 0; n < 2; n++) {
			f.src_reg[n] = r.value;
			f.src_is_temp[n] = r.kind == XSC_OPERAND_REG;
		}
		if (!xsdk_xsc_program_export(l->x, l->p, use->a, use->b, &f))
			return false;
	}
	return true;
}

static bool is_sample(uint32_t opcode)
{
	return opcode == SPV_IMAGE_SAMPLE_IMPLICIT_LOD || opcode == SPV_IMAGE_SAMPLE_EXPLICIT_LOD ||
		opcode == SPV_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD || opcode == SPV_IMAGE_SAMPLE_PROJ_EXPLICIT_LOD;
}

// Every sample, after the vertex fetches and before the exports, as a pixel shader gathers its samples first, but one
// at what a hardware loop's pass works out, lowered in the pass (xsc_in_pass); the fetch constants they read marked.
static bool sample_all(XscLower *l, bool used[FETCH_SLOTS])
{
	for (size_t i = 0; i < l->m->instr_count; i++) {
		const XscInstr *const ins = &l->m->instrs[i];
		if (!is_sample(ins->opcode))
			continue;
		int64_t slot;
		const bool ok = xsc_in_pass(l, ins->result_id) ? xsdk_xsc_sample_slot(l, ins, &slot)
													   : xsdk_xsc_emit_sample(l, ins->result_id, ins, &slot);
		if (!ok)
			return false;
		if (slot < 0 || slot >= FETCH_SLOTS)
			return xsdk_xsc_lower_refuse(l, "a sampler at fetch constant %lld, past 31", (long long)slot);
		used[slot] = true;
	}
	return true;
}

// The samplers first, as a pixel shader's container has them, then the uniforms, the instance group and the literals.
static bool add_constants(XscLower *l, const bool used[FETCH_SLOTS], int64_t group, XscConstants *constants)
{
	for (int64_t slot = 0; slot < FETCH_SLOTS; slot++) {
		const XscConstant c = used[slot] ? xsdk_xsc_sampler_constant(l, slot) : (XscConstant){ 0 };
		if (used[slot] && (!c.name || !xsdk_xsc_add_constant(l->x, constants, c)))
			return false;
	}
	if (!xsdk_xsc_uniform_constants(l, constants, true))
		return false;
	if (group != XSC_NONE && !xsdk_xsc_add_constant(l->x, constants, xsdk_xsc_vector_constant("_instance", group, 1)))
		return false;
	return xsdk_xsc_literal_constants(l, constants);
}

// The position and the point's size, then the interpolators; without any, they're allocated first, as xsdk's resolve
// shader is.
static bool lower_body(
	XscLower *l, const XscInterface *si, const PerVertex *pv, const XscStores *stores, uint32_t position)
{
	if (!si->interp_out_count && !xsdk_xsc_program_alloc_interpolators(l->x, l->p))
		return false;
	if (!xsdk_xsc_program_alloc(l->x, l->p, XSC_ALLOC_POSITION))
		return false;
	if (!position)
		return xsdk_xsc_lower_refuse(l, "no write to gl_Position");
	if (!xsdk_xsc_emit_value(l, position, XSC_EXPORT_POSITION, true, true) || !export_point_size(l, pv, stores))
		return false;
	if (si->interp_out_count && !xsdk_xsc_program_alloc_interpolators(l->x, l->p))
		return false;
	return export_interpolators(l, si, stores);
}

// A value through the loads of the locals it was stored in, as the lowering reads them.
static uint32_t stored_value(const XscLower *l, uint32_t ssa)
{
	for (uint32_t depth = 0; depth < XSC_DEPTH_LIMIT; depth++) {
		const XscInstr *const load = xsdk_xsc_by_result(l->m, ssa);
		const uint32_t pointer = load && load->opcode == SPV_LOAD ? xsdk_xsc_operand(load, 0) : 0;
		if (!pointer || pointer >= l->m->id_limit || !l->local_store[pointer])
			break;
		ssa = l->local_store[pointer];
	}
	return ssa;
}

// Whether a value is gl_Position's: the value stored into it, or a load of it.
static bool is_position(XscLower *l, const PerVertex *pv, uint32_t ssa, uint32_t position)
{
	const uint32_t value = stored_value(l, ssa);
	if (value == stored_value(l, position))
		return true;
	const XscInstr *const load = xsdk_xsc_by_result(l->m, value);
	return load && load->opcode == SPV_LOAD && is_position_pointer(l, pv, xsdk_xsc_operand(load, 0));
}

// The register of the plane a clip distance reads: its value is dot(gl_Position, plane), either way round, the plane a
// whole vec4 of a uniform or push constant block at a fixed index.
static bool plane_register(XscLower *l, const PerVertex *pv, uint32_t value, uint32_t position, int64_t *reg)
{
	const XscInstr *const dot = xsdk_xsc_by_result(l->m, stored_value(l, value));
	const uint32_t a = dot && dot->opcode == SPV_DOT ? xsdk_xsc_operand(dot, 0) : 0;
	const uint32_t b = a ? xsdk_xsc_operand(dot, 1) : 0;
	const uint32_t plane = !a ? 0 : is_position(l, pv, a, position) ? b : is_position(l, pv, b, position) ? a : 0;
	const XscInstr *const load = plane ? xsdk_xsc_by_result(l->m, stored_value(l, plane)) : NULL;
	XscBlockAccess access;
	bool found = false;
	if (load && load->opcode == SPV_LOAD && !xsdk_xsc_block_access(l, xsdk_xsc_operand(load, 0), &access, &found))
		return false;
	const XscType *const t = found ? xsdk_xsc_type(l->m, l->types, access.tid) : NULL;
	if (!t || t->kind != XSC_TYPE_VECTOR || t->count != 4 || access.comp || access.dynamic)
		return xsdk_xsc_lower_refuse(l,
			"gl_ClipDistance other than dot(gl_Position, a plane), the plane a whole vec4 of a block at a fixed "
			"index, as Xenos's user clip planes take it (v1)");
	*reg = access.reg;
	return true;
}

// gl_ClipDistance[n] = dot(gl_Position, plane) is Xenos's user clip plane n, which clips against the exported position
// itself (xenia:src/xenia/gpu/dxbc_shader_translator.cc:1033-1053), so it costs no instruction: the runtime sets the
// plane from its register. Xenos has no cull distances.
static bool clip_planes(XscLower *l, const PerVertex *pv, const XscStores *stores, uint32_t position, XscLowered *out)
{
	uint32_t written = 0;
	for (size_t i = 0; i < stores->target_count && pv->position != XSC_NONE; i++) {
		const uint32_t target = stores->targets[i];
		const int64_t member = member_of(l, target);
		if (xsc_failed(l))
			return false;
		if (member != XSC_NONE && member == pv->cull_distance)
			return xsdk_xsc_lower_refuse(l, "gl_CullDistance: Xenos has no cull distances");
		if (member == XSC_NONE || member != pv->clip_distance)
			continue;
		const XscInstr *const chain = xsdk_xsc_by_result(l->m, target);
		int64_t n = XSC_NONE;
		if (xsdk_xsc_operand_count(chain) != 3 || !xsdk_xsc_int_const(l, xsdk_xsc_operand(chain, 2), &n) || n < 0 ||
			n >= CLIP_PLANES || (written & 1u << n))
			return xsdk_xsc_lower_refuse(
				l, "gl_ClipDistance other than elements 0 to 5 each written once at a constant index (v1)");
		int64_t reg = 0;
		if (!plane_register(l, pv, stores->value[target], position, &reg))
			return false;
		written |= 1u << n;
		const XsdkXscNeed need = { .kind = XSDK_XSC_CLIP_PLANE, .reg = (uint32_t)reg, .regs = (uint32_t)n };
		if (!xsdk_xsc_add_need(l->x, out, need))
			return false;
	}
	return true;
}

// An output decorated as a gl_PerVertex member but standing alone would be dropped, so it's refused; HLSL's PSIZE is
// exported (xsc_resolve.c).
static bool check_lone_outputs(Xsc *x, const XscModule *m, const XscInterface *si)
{
	for (size_t i = 0; i < m->instr_count; i++) {
		const XscInstr *const ins = &m->instrs[i];
		if (ins->opcode != SPV_VARIABLE || xsdk_xsc_operand(ins, 0) != STORAGE_OUTPUT ||
			(si->point_size && ins->result_id == si->point_size->vid))
			continue;
		const int64_t builtin =
			xsdk_xsc_decoration_arg(xsdk_xsc_decoration(m, ins->result_id, SPV_DEC_BUILTIN), XSC_NONE);
		if (builtin == BUILTIN_POINT_SIZE || builtin == BUILTIN_CLIP_DISTANCE || builtin == BUILTIN_CULL_DISTANCE)
			return xsdk_xsc_fail(
				x, XSDK_XSC_REFUSED, "gl_PointSize, gl_ClipDistance or gl_CullDistance outside gl_PerVertex (v1)");
	}
	return true;
}

bool xsdk_xsc_lower_vertex(Xsc *x, const XscModule *m, const XsdkXscOptions *options, XscInterface *si, XscLowered *out)
{
	if (!xsdk_xsc_resolve_interface(x, m, si))
		return false;
	if (si->pixel)
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "lower_vertex called on non-vertex module");
	if (has_branches(m))
		return xsdk_xsc_fail(x, XSDK_XSC_REFUSED, "vertex shader control flow the unroller left isn't lowered");
	if (!check_lone_outputs(x, m, si))
		return false;
	const XscType *const types = xsdk_xsc_resolve_types(x, m);
	XscStores stores;
	XscProgram p;
	if (!types || !xsdk_xsc_store_map(x, m, &stores) || !xsdk_xsc_program_init(x, &p, false, options->vertex_registers))
		return false;
	p.optimize = options->optimize;
	// Temps run past r63 till the allocator packs them.
	p.whole_sources = true;
	const int64_t index_group = xsdk_xsc_instance_index_reg(si);
	// The option's group takes the register past what the shader declares, as gl_InstanceIndex's does.
	out->instance = index_group == XSC_NONE && options->instanced ? si->constant_end++ : index_group;
	// An instanced shader's fetches leave r0, the vertex index its prologue reads, as the hardware filled it.
	const int64_t first = out->instance != XSC_NONE;
	XscLower l;
	if (!xsdk_xsc_lower_init(
			x, &l, m, si, &p, types, true, first + (int64_t)si->vfetch_count, xsdk_xsc_literal_base(si)))
		return false;
	const int64_t index = first ? prologue(&l, out->instance, index_group != XSC_NONE) : 0;
	if (index == XSC_NONE || !fetch_inputs(&l, si, index, first))
		return false;
	bool *const locals = xsdk_xsc_local_vars(x, m, 0, m->instr_count);
	if (!locals || !xsdk_xsc_check_local_stores(x, m, locals, 0, 0))
		return false;
	xsdk_xsc_fill_local_store(&l, &stores, locals);
	bool used[FETCH_SLOTS] = { false };
	XscConstants constants = { 0 };
	const PerVertex pv = per_vertex(m, types, si);
	const uint32_t position = position_value(&l, &pv, &stores);
	return !xsc_failed(&l) && sample_all(&l, used) && lower_body(&l, si, &pv, &stores, position) &&
		add_constants(&l, used, out->instance, &constants) &&
		xsdk_xsc_program_finish(x, &p, constants.items, constants.count, &out->container, &out->container_bytes) &&
		xsdk_xsc_literal_needs(&l, out) && xsdk_xsc_loop_needs(&l, out) && clip_planes(&l, &pv, &stores, position, out);
}
