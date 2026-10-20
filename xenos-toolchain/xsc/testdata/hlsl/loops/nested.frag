#version 450
// nested_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Scale; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float s = 0.0;
    float t = 0.0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            s += uv.x * float(i) + uv.y * float(j) * g.Scale.x;
            if (s > g.Scale.y)
                break;
        }
        t += s;
    }
    color = vec4(s, t, uv.z, 1.0);
}
