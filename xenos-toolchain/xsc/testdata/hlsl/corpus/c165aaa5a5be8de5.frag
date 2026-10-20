#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 sampled = texture(source, in_uv);
    vec4 c = sampled;
    o_color = c.rrrg;
}
