#version 450
// A discard after an early return the unroller takes, as a local is stored again: a pixel that returned lives.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 colour;
layout(location = 0) out vec4 o;

void main()
{
    vec4 c = colour;
    c.g = uv.y;
    if (uv.x > 0.5) {
        o = c;
        return;
    }
    discard;
}
