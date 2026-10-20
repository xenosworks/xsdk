#version 450
layout(set = 0, binding = 0, std140) uniform View {
    vec4 viewport_sizes; // render, then logical
    vec4 line_params;
    vec4 proj[4];
    vec4 tex_mtx[30];
    vec4 channel_colors[8]; // per channel: ambient, material, alpha ambient, alpha material
    vec4 postmtx[60];
    vec4 fog[5];
    vec4 ind_mtx[6];
} uview;
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec3 in_tex0_uvw;
layout(location = 2) in vec2 in_tex1_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uvw.xy / in_tex0_uvw.z;
    vec2 tex1_uv = in_tex1_uv;
    // Indirect stage 0
    vec3 ind_tex0 = 255.0 * texture(tex1, tex1_uv * umat.texcoord_scale[1].xy * vec2(1.0, 1.0) / umat.tex_size_bias[1].xy).abg;
    // TEV stage 0 indirect
    vec3 ind0_coord = ind_tex0;
    vec3 ind0_biased = ind0_coord + vec3(1.0, 1.0, 1.0) * -128.0;
    vec4 ind0_c0 = uview.ind_mtx[0];
    vec4 ind0_c1 = uview.ind_mtx[1];
    vec2 ind0_texel = tex0_uv * umat.texcoord_scale[0].xy;
    vec2 tev_coord0 = ind0_texel + (vec2(dot(vec3(ind0_c0.xz, ind0_c1.x), ind0_biased), dot(vec3(ind0_c0.yw, ind0_c1.y), ind0_biased)) * ind0_c1.z);
    vec2 ind0_uv = tev_coord0 / umat.tex_size_bias[0].xy;
    vec4 sampled0 = texture(tex0, ind0_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (vec3(1.0)), (sampled0.rgb)) + vec3(0.0))), -4.0, 4.0);
    float s0_alpha = clamp(((mix((0.0), (umat.kcolor[3].a), (in_cc0.a)) + 0.0)), 0.0, 1.0);
    vec3 out_color = ((s0_color) * 255.0 - floor((s0_color) * 255.0 / 256.0) * 256.0) / 255.0;
    o_color = vec4(out_color, s0_alpha);
}
