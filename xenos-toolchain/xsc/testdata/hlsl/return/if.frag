#version 450
// if_ps.hlsl's GLSL twin.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 o;

void main()
{
    vec4 c = colour;
    if (uv.x > 0.5) {
        o = vec4(uv, 0.25, 1.0);
        return;
    }
    c.g = uv.y;
    o = c;
}
