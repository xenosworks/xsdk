#version 450
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(location = 0) in vec2 in_tex0_uv;
layout(location = 1) in vec2 in_tex1_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec2 tex1_uv = in_tex1_uv;
    vec4 sampled0 = texture(tex0, tex0_uv);
    vec4 sampled1 = texture(tex1, tex1_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((sampled0.rgb), (vec3(0.0)), (vec3(0.0))) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((sampled0.a), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // TEV stage 1
    vec3 s1_color = clamp(((mix((vec3(0.0)), (sampled1.rgb), (s0_color)) + vec3(0.0))), 0.0, 1.0);
    float s1_alpha = clamp(((mix((sampled1.a), (0.0), (0.0)) + s0_alpha)), 0.0, 1.0);
    // TEV stage 2
    vec3 s2_color = clamp(((mix((umat.tevreg[1].rgb), (umat.kcolor[0].rgb), (s1_color)) + vec3(0.0))), 0.0, 1.0);
    float s2_alpha = clamp(((mix((0.0), (umat.tevreg[1].a), (s1_alpha)) + 0.0)), 0.0, 1.0);
    o_color = vec4(s2_color, s2_alpha);
}
