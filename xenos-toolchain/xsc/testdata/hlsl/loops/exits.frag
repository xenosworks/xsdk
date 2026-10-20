#version 450
// exits_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Threshold; int Count; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float s = 0.0;
    float last = 0.0;
    float found = -1.0;
    for (int i = 0; i < g.Count; i++) {
        last = s;
        s += uv.x + float(i) * 0.25;
        if (s > g.Threshold.x) {
            found = float(i);
            break;
        }
    }
    color = vec4(s, last, found, 1.0);
}
