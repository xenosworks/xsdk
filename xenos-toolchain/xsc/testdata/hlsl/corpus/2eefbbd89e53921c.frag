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
    vec4 depth_range; // the render viewport's znear and zfar
} uview;
layout(set = 0, binding = 4, std140) uniform Material {
    vec4 tevreg[4]; // TEVPREV, then TEVREG0-2
    vec4 kcolor[4];
    vec4 texcoord_scale[8];
    vec4 tex_size_bias[8];
} umat;
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(set = 1, binding = 2) uniform sampler2D tex2;
layout(set = 1, binding = 3) uniform sampler2D tex3;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec2 in_tex0_uv;
layout(location = 2) in vec2 in_tex1_uv;
layout(location = 3) in vec2 in_tex2_uv;
layout(location = 4) in vec2 in_tex3_uv;
layout(location = 5) in vec2 in_fog_zw;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec2 tex1_uv = in_tex1_uv;
    vec2 tex2_uv = in_tex2_uv;
    vec2 tex3_uv = in_tex3_uv;
    // Indirect stage 0
    vec3 ind_tex0 = 255.0 * texture(tex3, tex3_uv * umat.texcoord_scale[3].xy * vec2(1.0, 1.0) / umat.tex_size_bias[3].xy).abg;
    vec4 sampled0 = texture(tex1, tex1_uv);
    vec4 sampled1 = texture(tex0, tex0_uv);
    // TEV stage 2 indirect
    vec3 ind2_coord = ind_tex0;
    vec3 ind2_biased = ind2_coord + vec3(1.0, 1.0, 1.0) * -128.0;
    vec4 ind2_c0 = uview.ind_mtx[0];
    vec4 ind2_c1 = uview.ind_mtx[1];
    vec2 ind2_texel = tex2_uv * umat.texcoord_scale[2].xy;
    vec2 tev_coord2 = ind2_texel + (vec2(dot(vec3(ind2_c0.xz, ind2_c1.x), ind2_biased), dot(vec3(ind2_c0.yw, ind2_c1.y), ind2_biased)) * ind2_c1.z);
    vec2 ind2_uv = tev_coord2 / umat.tex_size_bias[2].xy;
    vec4 sampled2 = texture(tex2, ind2_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((sampled0.rgb), (vec3(0.0)), (vec3(0.0))) + vec3(0.0))) / 2.0, 0.0, 1.0);
    float s0_alpha = clamp(((mix((sampled0.a), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // TEV stage 1
    vec3 s1_color = clamp(((mix((s0_color), (sampled1.rgb), (vec3(umat.kcolor[1].a))) + vec3(0.0))), 0.0, 1.0);
    float s1_alpha = clamp(((mix((sampled1.a), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // TEV stage 2
    vec3 s2_color = clamp(((mix((vec3(0.0)), (umat.tevreg[2].rgb), (sampled2.rgb)) + s1_color)), 0.0, 1.0);
    float s2_alpha = clamp(((mix((0.0), (in_cc0.a), (s1_alpha)) + 0.0)), 0.0, 1.0);
    // Fog
    float fog_ndc = in_fog_zw.x / in_fog_zw.y;
    float fog_depth = uview.depth_range.y - fog_ndc * (uview.depth_range.y - uview.depth_range.x);
    float fog_base = uview.fog[1].x / (uview.fog[1].y - fog_depth);
    float fog_f = clamp(fog_base - uview.fog[1].z, 0.0, 1.0);
    float fog_z = fog_f;
    vec3 fog_color = mix(s2_color, uview.fog[0].rgb, clamp(fog_z, 0.0, 1.0));
    o_color = vec4(fog_color, s2_alpha);
}
