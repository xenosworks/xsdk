#version 450
// bool_ps.hlsl's twin, the bool a float 0 or 1, as GLSL's bools in a block aren't D3D9's floats.
layout(set = 0, binding = 0) uniform Globals { float UseTint; vec4 Tint; } g;
layout(location = 0) in vec4 a;
layout(location = 1) in vec4 b;
layout(location = 0) out vec4 color;
const bvec4 Mask = bvec4(true, false, true, false);

void main()
{
    vec4 c = mix(b, a, Mask);
    vec4 t = g.UseTint > 0.0 ? g.Tint : vec4(1.0);
    bvec4 m = bvec4(true, false, a.x > 0.5, true);
    vec3 c3 = mix(b.xyz + 1.0, a.xyz * 2.0, Mask.xyz);
    color = mix(b, c, m) * t + dot(c3, vec3(1.0, 2.0, 4.0));
}
