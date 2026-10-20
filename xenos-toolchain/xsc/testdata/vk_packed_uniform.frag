#version 450
// A colour and its alpha clamped from a uniform apart, as Dusklight's 1e5db8674ad9d29e.frag: one instruction that names
// no temp.
layout(set = 0, binding = 0, std140) uniform Material {
    vec4 tevreg[2];
} umat;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 color = clamp(umat.tevreg[1].rgb, 0.0, 1.0);
    float alpha = clamp(umat.tevreg[1].a, 0.0, 1.0);
    o_color = vec4(color, alpha);
}
