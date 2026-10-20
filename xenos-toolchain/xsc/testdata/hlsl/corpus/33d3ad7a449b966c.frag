#version 450
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(location = 0) in vec4 in_cc0;
layout(location = 0) out vec4 o_color;
void main() {
    // TEV stage 0
    vec3 s0_color = clamp(((mix((umat.tevreg[1].rgb), (vec3(0.0)), (vec3(0.0))) + vec3(0.0))), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (umat.tevreg[1].a), (in_cc0.a)) + 0.0)), 0.0, 1.0);
    o_color = vec4(s0_color, s0_alpha);
}
