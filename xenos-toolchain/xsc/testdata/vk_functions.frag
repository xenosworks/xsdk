#version 450
// A helper of a helper inlined into a pixel shader, each called with the other's result (tests/test_functions.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

vec4 blend(vec4 x, vec4 y, float t)
{
	return mix(x, y, t);
}

vec4 shade(vec2 uv)
{
	vec4 c = blend(pc.a, pc.b, uv.x);
	c.w = uv.y;
	return c;
}

void main()
{
	o_color = blend(shade(v_uv), pc.a, 0.25);
}
