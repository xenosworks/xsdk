#version 450
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec4 in_cc1;
layout(location = 2) in vec2 in_tex0_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec4 sampled0 = texture(tex0, tex0_uv);
    vec4 sampled1 = texture(tex0, tex0_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (in_cc0.rgb), (sampled0.rgb)) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (sampled0.a), (in_cc0.a)) + 0.0)), 0.0, 1.0);
    // TEV stage 1
    vec3 s1_color = clamp(((mix((in_cc1.rgb), (vec3(0.0)), (sampled1.rgb)) + s0_color)), 0.0, 1.0);
    float s1_alpha = clamp(((mix((0.0), (0.0), (0.0)) + s0_alpha)), 0.0, 1.0);
    o_color = vec4(s1_color, s1_alpha);
}
