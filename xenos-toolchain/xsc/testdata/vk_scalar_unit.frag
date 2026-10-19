#version 450
// Ops writing one value the optimiser co-issues in the scalar unit, and ones it can't (tests/test_scalar_unit.py).
layout(location = 0) in vec4 v_a;
layout(location = 1) in vec4 v_b;
layout(location = 0) out vec4 o_one;
layout(location = 1) out vec4 o_constant;
layout(location = 2) out vec4 o_order;
layout(location = 3) out vec4 o_kept;

void main()
{
	// Each beside a dot product, which has no scalar form. Operands in one register: adds, subs, subs of a negation,
	// muls, a square, and floors and frcs of sums.
	vec4 one = vec4(v_a.x + v_a.y, dot(v_a, v_b), v_a.z - v_a.w, dot(v_a, v_b.yzwx));
	vec4 two = vec4((0.0 - v_a.x) + v_a.z, dot(v_a, v_b.zwxy), v_a.y * v_a.w, dot(v_a, v_b.wxyz));
	vec4 three = vec4(v_a.y * v_a.y, dot(v_a.yzwx, v_b), floor(v_b.x + v_a.y), dot(v_a.zwxy, v_b));
	vec4 six = vec4(fract(v_b.y + v_a.z), dot(v_a.xzwy, v_b), dot(v_a, v_b.xzwy), dot(v_a.ywzx, v_b.wzyx));
	// A constant and a temp: mulsc, addsc, subsc and subsc of a negation, of an input and of sums, whose register
	// numbers spread over the opcode, src3_sel and src3's swizzle.
	vec4 four = vec4(2.5 * v_b.w, dot(v_a.wxyz, v_b), 0.75 + v_b.x, dot(v_a.yzwx, v_b.zwxy));
	vec4 five = vec4(3.0 - v_b.y, dot(v_a.zwxy, v_b.yzwx), v_b.z - 1.5, dot(v_a.wxyz, v_b.zwxy));
	vec4 sums = v_a + v_b.yzwx;
	vec4 seven = vec4(2.5 * sums.w, dot(sums, v_b), 0.75 + sums.x, dot(sums, v_a));
	vec4 eight = vec4(3.0 - sums.y, dot(sums.yzwx, v_b), sums.z - 1.5, dot(sums.zwxy, v_a));
	// maxs and mins, whose operands' order NaN and -0 show.
	vec4 order = vec4(max(v_a.x, v_a.w), dot(v_a.yzwx, v_b.wxyz), min(v_a.y, v_a.z), dot(v_a.zwxy, v_b.wxyz));
	// Not scalar: two registers, one of two operands negated, a max with a constant, one of two absolute, and a dot
	// product with a literal 1 in lane x, which dp2add would add last.
	vec4 kept = vec4(v_a.x * v_b.x, (0.0 - v_a.x) * v_a.y, max(v_b.w, 0.5), abs(v_a.x) + v_a.y);
	float d = dot(vec3(1.0, v_a.z, v_a.w), v_b.xyz);
	// Adds alone, as the optimiser fuses a multiply and an add, rounding once.
	o_one = one + two;
	o_constant = four + five + seven * eight;
	o_order = order * v_b;
	o_kept = kept + three + six + d;
}
