#version 450
// Helpers inlined: one called three times, one calling another, an inout and an out parameter, and a store into part
// of a parameter (tests/test_functions.py).
layout(location = 0) in vec4 a_position;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

float wave(float x)
{
	return x * 0.5 + 0.25;
}

vec2 twice(vec2 v, float k)
{
	v.y = wave(v.y) * k;
	return v + vec2(wave(v.x));
}

void scale(inout vec4 v, float k, out float sum)
{
	v.xz *= k;
	sum = v.x + v.z;
}

void main()
{
	vec4 p = a_position;
	float sum;
	scale(p, pc.a.x, sum);
	p.xy = twice(p.xy, pc.b.y) + twice(pc.a.zw, sum);
	p.w = wave(pc.b.w);
	gl_Position = p;
}
