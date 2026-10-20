#version 450
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec2 in_tex0_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec4 sampled0 = texture(tex0, tex0_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((umat.tevreg[2].rgb), (umat.tevreg[1].rgb), (sampled0.rgb)) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (sampled0.a), (umat.tevreg[1].a)) + 0.0)), 0.0, 1.0);
    o_color = vec4(s0_color, s0_alpha);
}
