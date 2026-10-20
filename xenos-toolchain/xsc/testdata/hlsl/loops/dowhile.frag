#version 450
// dowhile_ps.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Limit; } g;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

void main()
{
    float x = uv.x;
    int n = 0;
    do {
        x = x * 1.5 + 0.25;
        n++;
    } while (x < g.Limit.x);
    color = vec4(x, float(n), uv.y, 1.0);
}
