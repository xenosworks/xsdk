#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 z24 = min(floor(clamp(1.0 - texture(source, in_uv).rrrr, 0.0, 1.0) * 16777215.0 + 0.5), 16777215.0);
    vec4 high = floor(z24 * (1.0 / 65536.0));
    o_color = high * (1.0 / 255.0);
}
