#version 450
// A discard under an if, then an if changing a value: a kill-if beside another if/else.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

void main()
{
    vec4 c = vec4(uv, 0, 1);
    if (uv.x > 0.5)
        discard;
    if (uv.y > 0.5)
        c.r = 1.0;
    o = c;
}
