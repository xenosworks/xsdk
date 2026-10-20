#version 450
// pow with an exponent that differs from lane to lane: each lane takes its own (test_pow_lanes.py).
layout(location = 0) in vec4 v_base;
layout(location = 1) in vec4 v_power;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(pow(v_base.xyz, v_power.xyz), 1.0);
}
