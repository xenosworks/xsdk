#version 450
// Division by constants, by one scalar and by a varying vector (tests/test_division.py).
layout(location = 0) in vec4 v_a;
layout(location = 1) in vec4 v_b;
layout(location = 0) out vec4 o_by_constant;
layout(location = 1) out vec4 o_by_constants;
layout(location = 2) out vec4 o_by_scalar;
layout(location = 3) out vec4 o_by_vector;

void main()
{
	o_by_constant = vec4(v_a.xyz / 255.0, v_b.x / 7.0);
	o_by_constants = v_a / vec4(3.0, -5.0, 0.1, 256.0);
	o_by_scalar = v_a / v_b.w;
	// A divisor with a lane of 0 keeps the GPU's rcp in every lane, as a varying one does.
	o_by_vector = vec4(v_a.xy / v_b.xy, v_a.zw / vec2(0.0, 4.0));
}
