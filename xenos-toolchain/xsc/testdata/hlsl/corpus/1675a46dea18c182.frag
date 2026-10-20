#version 450
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec2 in_tex0_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec4 sampled0 = texture(tex0, tex0_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((sampled0.rgb), (vec3(0.0)), (vec3(0.0))) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((sampled0.a), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // Alpha compare
    float alpha_byte = floor(s0_alpha * 255.0 + 0.5);
    float alpha_pass = step(0.5, alpha_byte - 0.0);
    if (alpha_pass < 0.5) {
        discard;
    }
    o_color = vec4(s0_color, s0_alpha);
}
