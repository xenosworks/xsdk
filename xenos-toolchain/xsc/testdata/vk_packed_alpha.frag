#version 450
// A colour and its alpha exported apart, as Dusklight's GX shaders export them (15a57b221b955025.frag): the alpha
// compare reads the alpha in every lane, so the alpha stays in its temp and each export copies one value of it.
layout(set = 0, binding = 0, std140) uniform Material {
    vec4 fog_color;
} umat;
layout(location = 0) in vec4 in_c;
layout(location = 1) in vec4 in_d;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_fogged;
void main() {
    float alpha = clamp(in_c.a * in_d.a, 0.0, 1.0);
    float alpha_byte = floor(alpha * 255.0 + 0.5);
    float alpha_pass = step(0.5, alpha_byte - 128.0);
    if (alpha_pass < 0.5) {
        discard;
    }
    o_color = vec4(in_c.rgb * in_d.rgb, alpha);
    o_fogged = vec4(mix(in_c.rgb, umat.fog_color.rgb, in_d.x), alpha);
}
