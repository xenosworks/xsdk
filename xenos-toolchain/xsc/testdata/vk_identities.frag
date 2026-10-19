#version 450
// mix with a constant 0 or 1, and adds, muls and mads with a known 0 or 1, each to what remains
// (tests/test_identities.py).
layout(location = 0) in vec4 v_x;
layout(location = 1) in vec4 v_y;
layout(location = 2) in vec4 v_a;
layout(location = 0) out vec4 o_weight_one;
layout(location = 1) out vec4 o_mixes;
layout(location = 2) out vec4 o_identities;
layout(location = 3) out vec4 o_arising;

void main()
{
	o_weight_one = mix(v_x, v_y, vec4(1.0));
	o_mixes = mix(vec4(0.0), v_y, v_a) + mix(v_x, vec4(0.0), v_a) + mix(v_x, v_y, vec4(0.0));
	o_identities = v_x * vec4(1.0) + (vec4(0.0) - v_y) + v_a * vec4(0.0);
	// The 0 and 1 are known only once the optimiser reads them through the vector they're built into.
	vec4 t = vec4(v_x.xy, 0.0, 1.0);
	o_arising = mix(t.zzzz, v_y, v_a) + t.wwww * v_x + mix(v_y, v_x, t.zzzz);
}
