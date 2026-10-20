#version 450
// switch_ps.hlsl's GLSL twin.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 o;

void main()
{
    vec4 c = colour;
    switch (int(uv.x * 8.0)) {
    case 0:
        c.r = 0.25;
        break;
    case 2:
    case 3:
        c.g = uv.y;
        break;
    case 5:
        c.b = 0.75;
        c.a = 0.5;
        break;
    default:
        c *= 0.5;
        break;
    }
    o = c;
}
