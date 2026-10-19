#version 450
// 1e-10 and 0 as literals side by side: each must keep a register of its own (tests/test_literal_pool.py).
layout(location = 0) in vec4 v_value;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(max(v_value.x, 1e-10), step(1e-10, v_value.y), max(v_value.z, 0.0), 1.0);
}
