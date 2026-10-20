#version 450
// while_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Limit; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float x = uv.x;
    int i = 0;
    while (i < 6) {
        i++;
        x = x * 1.5 + 0.25;
        if (x > g.Limit.x)
            break;
    }
    color = vec4(x, float(i), uv.y, 1.0);
}
