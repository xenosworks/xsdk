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
    vec4 sampled0 = texture(tex2, tex2_uv);
    vec4 sampled1 = texture(tex0, tex0_uv);
    vec4 sampled2 = texture(tex1, tex1_uv);
    // TEV stage 0
    vec3 s0_color = clamp(((mix((vec3(0.0)), (sampled0.rgb), (in_cc0.rgb)) + umat.tevreg[2].rgb)), 0.0, 1.0);
    float s0_alpha = clamp(((mix((0.0), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // TEV stage 1
    vec3 s1_color = clamp(((mix((vec3(0.0)), (sampled1.rgb), (s0_color)) + vec3(0.0))), 0.0, 1.0);
    float s1_alpha = clamp(((mix((0.0), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // TEV stage 2
    vec3 s2_color = clamp(((mix((vec3(0.0)), (in_cc0.rgb), (sampled2.rgb)) + s1_color)) * 4.0, 0.0, 1.0);
    float s2_alpha = clamp(((mix((umat.kcolor[3].a), (0.0), (0.0)) + 0.0)), 0.0, 1.0);
    // Fog
    float fog_ndc = in_fog_zw.x / in_fog_zw.y;
    float fog_depth = uview.depth_range.y - fog_ndc * (uview.depth_range.y - uview.depth_range.x);
    float fog_base = uview.fog[1].x / (uview.fog[1].y - fog_depth);
    float fog_column = gl_FragCoord.x * (1.0 / 64.0);
    vec4 fog_range = texture(fog_range_table, vec2(fog_column, floor(fog_column) * (1.0 / 32.0) + (0.5 / 32.0)));
    float fog_f = clamp(fog_base * fog_range.x - uview.fog[1].z, 0.0, 1.0);
    float fog_z = fog_f;
    vec3 fog_color = mix(s2_color, uview.fog[0].rgb, clamp(fog_z, 0.0, 1.0));
    o_color = vec4(fog_color, s2_alpha);
}
