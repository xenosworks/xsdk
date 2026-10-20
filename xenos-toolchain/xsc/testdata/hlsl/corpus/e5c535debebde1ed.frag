#version 450
layout(location = 0) in vec4 in_cc0;
layout(location = 0) out vec4 o_color;
void main() {
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (vec3(0.0)), (vec3(0.0))) + in_cc0.rgb)), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (0.0), (0.0)) + in_cc0.a)), 0.0, 1.0);
    o_color = vec4(s0_color, s0_alpha);
}
