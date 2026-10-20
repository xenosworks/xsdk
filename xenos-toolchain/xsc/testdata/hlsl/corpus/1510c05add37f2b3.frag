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
layout(set = 2, binding = 0) uniform sampler2D fog_range_table;
layout(location = 0) in vec4 in_cc0;
layout(location = 1) in vec2 in_tex0_uv;
layout(location = 2) in vec2 in_tex1_uv;
layout(location = 3) in vec2 in_tex2_uv;
layout(location = 4) in vec2 in_fog_zw;
layout(location = 0) out vec4 o_color;
void main() {
    vec2 tex0_uv = in_tex0_uv;
    vec2 tex1_uv = in_tex1_uv;
    vec2 tex2_uv = in_tex2_uv;
    // Indirect stage 0
    vec3 ind_tex0 = 255.0 * texture(tex2, tex2_uv * umat.texcoord_scale[2].xy * vec2(1.0, 1.0) / umat.tex_size_bias[2].xy).abg;
    // TEV stage 0 indirect
    vec3 ind0_coord = ind_tex0;
    vec3 ind0_biased = ind0_coord + vec3(1.0, 1.0, 1.0) * -128.0;
    vec4 ind0_c0 = uview.ind_mtx[0];
    vec4 ind0_c1 = uview.ind_mtx[1];
    vec2 ind0_texel = tex0_uv * umat.texcoord_scale[0].xy;
    vec2 tev_coord0 = ind0_texel + (vec2(dot(vec3(ind0_c0.xz, ind0_c1.x), ind0_biased), dot(vec3(ind0_c0.yw, ind0_c1.y), ind0_biased)) * ind0_c1.z);
    vec2 ind0_uv = tev_coord0 / umat.tex_size_bias[0].xy;
    vec4 sampled0 = texture(tex0, ind0_uv);
    // TEV stage 1 indirect
    vec3 ind1_coord = ind_tex0;
    vec3 ind1_biased = ind1_coord + vec3(1.0, 1.0, 1.0) * -128.0;
    vec4 ind1_c0 = uview.ind_mtx[0];
    vec4 ind1_c1 = uview.ind_mtx[1];
    vec2 ind1_texel = tex1_uv * umat.texcoord_scale[1].xy;
    vec2 tev_coord1 = ind1_texel + (vec2(dot(vec3(ind1_c0.xz, ind1_c1.x), ind1_biased), dot(vec3(ind1_c0.yw, ind1_c1.y), ind1_biased)) * ind1_c1.z);
    vec2 ind1_uv = tev_coord1 / umat.tex_size_bias[1].xy;
    vec4 sampled1 = texture(tex1, ind1_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (sampled0.rgb), (in_cc0.rgb)) + vec3(7.0 / 8.0))) / 2.0, -4.0, 4.0);
    float s0_alpha = clamp(((mix((0.0), (umat.kcolor[3].a), (in_cc0.a)) + 0.0)), 0.0, 1.0);
    vec3 s1_color_b = ((s0_color) * 255.0 - floor((s0_color) * 255.0 / 256.0) * 256.0) / 255.0;
    // TEV stage 1
    vec3 s1_color = clamp(((vec3(1.0)) * step(0.5, floor((sampled1.rgb).r * 255.0 + 0.5) - floor(s1_color_b.r * 255.0 + 0.5)) + umat.kcolor[1].rgb), 0.0, 1.0);
    float s1_alpha = clamp(((mix((0.0), (0.0), (0.0)) + s0_alpha)), 0.0, 1.0);
    // Fog
    float fog_ndc = in_fog_zw.x / in_fog_zw.y;
    float fog_depth = uview.depth_range.y - fog_ndc * (uview.depth_range.y - uview.depth_range.x);
    float fog_base = uview.fog[1].x / (uview.fog[1].y - fog_depth);
    float fog_column = gl_FragCoord.x * (1.0 / 64.0);
    vec4 fog_range = texture(fog_range_table, vec2(fog_column, floor(fog_column) * (1.0 / 32.0) + (0.5 / 32.0)));
    float fog_f = clamp(fog_base * fog_range.x - uview.fog[1].z, 0.0, 1.0);
    float fog_z = exp2(-8.0 * (1.0 - fog_f));
    vec3 fog_color = mix(s1_color, uview.fog[0].rgb, clamp(fog_z, 0.0, 1.0));
    o_color = vec4(fog_color, s1_alpha);
}
