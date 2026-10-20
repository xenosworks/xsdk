#version 450
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec2 in_tex0_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec4 sampled0 = texture(tex0, tex0_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((sampled0.rgb), (vec3(0.0)), (vec3(0.0))) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((umat.tevreg[3].a), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // TEV stage 1
    vec3 s1_color = clamp(((mix((vec3(0.0)), (s0_color), (in_cc0.rgb)) + vec3(0.0))), 0.0, 1.0);
    float s1_alpha = clamp(((mix((0.0), (s0_alpha), (in_cc0.a)) + 0.0)), 0.0, 1.0);
    // Alpha compare
    float alpha_byte = floor(s1_alpha * 255.0 + 0.5);
    float alpha_pass = step(0.5, alpha_byte - 0.0);
    if (alpha_pass < 0.5) {
        discard;
    }
    o_color = vec4(s1_color, s1_alpha);
}
