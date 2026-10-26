#version 450
// Helpers with control flow inlined, as PPSSPP's vertex shaders call normalizeOr001 (tests/test_functions.py): the
// conditional's arms are blocks of their own, and the call in the right-hand side of && is in the block the phi after
// it names.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

vec3 normalizeOr001(vec3 v)
{
	return length(v) == 0.0 ? vec3(0.0, 0.0, 1.0) : normalize(v);
}

void main()
{
	vec3 n = normalizeOr001(pc.a.xyz);
	vec3 z = normalizeOr001(pc.b.xyz);
	bool both = v_uv.x > 0.5 && normalizeOr001(pc.a.xyz * v_uv.y).x > 0.5;
	o_color = vec4(n.x + z.z, n.y, z.x, both ? 1.0 : 0.25);
}
