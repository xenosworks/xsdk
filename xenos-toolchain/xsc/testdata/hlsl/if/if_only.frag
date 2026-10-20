#version 450
// if_only_ps.hlsl's GLSL twin.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 o;

void main()
{
    vec4 c = colour;
    if (uv.x > 0.5)
        c.rgb *= 0.5;
    o = c;
}
