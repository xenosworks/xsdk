#version 450
// array_unrolled_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Scale; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;
const float Weights[5] = float[](0.5, 0.25, 0.125, 2.0, -1.0);
const vec2 Offsets[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));

void main()
{
    vec4 sum = uv * Weights[0];
    for (int i = 1; i < 5; i++)
        sum += uv.yzwx * Weights[i] * g.Scale.x;
    vec2 o = vec2(0.0);
    for (int j = 0; j < 4; j++)
        o += Offsets[j] * uv.xy * float(j + 1);
    color = vec4(sum.xy + o, sum.zw);
}
