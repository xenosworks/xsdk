#version 450
// uniform_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Step; vec4 Limit; int Count; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    vec4 s = uv;
    int i;
    for (i = 0; i < g.Count; i++) {
        s += g.Step;
        if (s.x > g.Limit.x)
            break;
    }
    color = vec4(s.xyz, float(i));
}
