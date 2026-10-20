#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 sampled = texture(source, in_uv);
    vec4 c = vec4(sampled.rgb, 1.0);
    float intensity = dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
    o_color = vec4(intensity);
}
