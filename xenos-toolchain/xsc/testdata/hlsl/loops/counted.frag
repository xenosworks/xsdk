#version 450
// counted_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Step; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    vec4 s = uv;
    for (int i = 0; i < 5; i++)
        s = s * 0.5 + g.Step * float(i);
    color = s;
}
