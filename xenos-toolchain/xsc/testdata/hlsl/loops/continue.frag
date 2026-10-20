#version 450
// continue_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Limit; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float s = 0.0;
    for (int i = 0; i < 6; i++) {
        float v = uv.x * float(i);
        if (v > g.Limit.x)
            continue;
        s += v;
    }
    color = vec4(s, uv.y, uv.z, uv.w);
}
