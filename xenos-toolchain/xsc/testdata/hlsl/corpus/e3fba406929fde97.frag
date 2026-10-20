#version 450
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec2 in_tex0_uv;
layout(location = 1) in vec2 in_tex1_uv;
layout(location = 2) in vec2 in_tex2_uv;
layout(location = 3) in vec2 in_tex3_uv;
layout(location = 4) in vec2 in_tex4_uv;
layout(location = 5) in vec2 in_tex5_uv;
layout(location = 6) in vec2 in_tex6_uv;
layout(location = 7) in vec2 in_tex7_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec2 tex1_uv = in_tex1_uv;
    vec2 tex2_uv = in_tex2_uv;
    vec2 tex3_uv = in_tex3_uv;
    vec2 tex4_uv = in_tex4_uv;
    vec2 tex5_uv = in_tex5_uv;
    vec2 tex6_uv = in_tex6_uv;
    vec2 tex7_uv = in_tex7_uv;
    vec4 sampled0 = texture(tex0, tex0_uv);
    vec4 sampled1 = texture(tex0, tex1_uv);
    vec4 sampled2 = texture(tex0, tex2_uv);
    vec4 sampled3 = texture(tex0, tex3_uv);
    vec4 sampled4 = texture(tex0, tex4_uv);
    vec4 sampled5 = texture(tex0, tex5_uv);
    vec4 sampled6 = texture(tex0, tex6_uv);
    vec4 sampled7 = texture(tex0, tex7_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (sampled0.rgb), (vec3(umat.tevreg[2].a))) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 1
    vec3 s1_color = clamp(((mix((vec3(0.0)), (sampled1.rgb), (vec3(umat.tevreg[2].a))) + s0_color)), 0.0, 1.0);
    float s1_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 2
    vec3 s2_color = clamp(((mix((vec3(0.0)), (sampled2.rgb), (vec3(umat.tevreg[2].a))) + s1_color)), 0.0, 1.0);
    float s2_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 3
    vec3 s3_color = clamp(((mix((vec3(0.0)), (sampled3.rgb), (vec3(umat.tevreg[2].a))) + s2_color)), 0.0, 1.0);
    float s3_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 4
    vec3 s4_color = clamp(((mix((vec3(0.0)), (sampled4.rgb), (vec3(umat.tevreg[2].a))) + s3_color)), 0.0, 1.0);
    float s4_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 5
    vec3 s5_color = clamp(((mix((vec3(0.0)), (sampled5.rgb), (vec3(umat.tevreg[2].a))) + s4_color)), 0.0, 1.0);
    float s5_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 6
    vec3 s6_color = clamp(((mix((vec3(0.0)), (sampled6.rgb), (vec3(umat.tevreg[2].a))) + s5_color)), 0.0, 1.0);
    float s6_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    // TEV stage 7
    vec3 s7_color = clamp(((mix((vec3(0.0)), (sampled7.rgb), (vec3(umat.tevreg[2].a))) + s6_color)), 0.0, 1.0);
    float s7_alpha = clamp(((mix((0.0), (0.0), (0.0)) + umat.tevreg[1].a)), 0.0, 1.0);
    o_color = vec4(s7_color, s7_alpha);
}
