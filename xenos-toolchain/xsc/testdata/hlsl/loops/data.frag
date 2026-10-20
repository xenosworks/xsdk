#version 450
// data_ps.hlsl's twin, t set before the loop, which GLSL's form needs to carry it out of a hardware loop.
layout(set = 0, binding = 0) uniform Globals { vec4 Limit; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float s = 0.0;
    float k = 1.0;
    float t = 0.0;
    while ((t = uv.x * k) < g.Limit.x) {
        s += t;
        k += 1.0;
    }
    color = vec4(s, t, k, 1.0);
}
