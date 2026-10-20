#version 450
// A discard after a return in an if inside another: every pixel the inner if doesn't return is killed, those whose y
// passes and whose x doesn't among them, which a kill on the first if alone would leave.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

void main()
{
    if (uv.y > 0.25) {
        if (uv.x > 0.5) {
            o = vec4(uv, 0, 1);
            return;
        }
    }
    discard;
}
