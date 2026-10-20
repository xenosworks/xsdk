#version 450
// break_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Threshold; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float s = 0.0;
    int i;
    for (i = 0; i < 8; i++) {
        s += uv.x + float(i) * 0.125;
        if (s > g.Threshold.x)
            break;
    }
    color = vec4(s, float(i), uv.y, 1.0);
}
