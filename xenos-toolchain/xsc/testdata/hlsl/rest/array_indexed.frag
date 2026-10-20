#version 450
// array_indexed_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Tint; int Count; } g;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
const vec4 Colours[6] = vec4[](vec4(0.5, 0.25, 0.125, 1), vec4(-1, 2, 0.75, 0.5), vec4(4, -0.5, 1, 0.25),
    vec4(0.0625, 8, -2, 1), vec4(3, 1.5, 0.5, -1), vec4(-0.25, -4, 6, 2));
const float Weights[6] = float[](1, 0.5, 2, -1, 0.25, 4);

void main()
{
    vec4 c = vec4(0.0);
    for (int i = 0; i < g.Count; i++)
        c += Colours[i] * Weights[i] + g.Tint * uv.x;
    int k = int(uv.y * 4.0);
    color = c + Colours[k] + Weights[k];
}
