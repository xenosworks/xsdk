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
layout(set = 0, binding = 3, std140) uniform Lights {
    vec4 lights[40];
    vec4 light_state;
} ulights;
layout(push_constant, std140) uniform Push {
    vec4 pos_mtx[3];
    vec4 nrm_mtx[3];
} upush;
layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec2 in_tex0_uv;
layout(location = 3) in vec2 in_tex1_uv;
layout(location = 4) in vec3 in_nrm;
layout(location = 0) out vec4 out_cc0;
layout(location = 1) out vec2 out_tex0_uv;
layout(location = 2) out vec2 out_tex1_uv;
layout(location = 3) out vec2 out_fog_zw;
void main() {
    vec4 pos4 = vec4(in_pos, 1.0);
    vec4 mv_pos4 = vec4(vec3(dot(upush.pos_mtx[0], pos4), dot(upush.pos_mtx[1], pos4), dot(upush.pos_mtx[2], pos4)), 1.0);
    vec4 clip_pos = vec4(dot(uview.proj[0], mv_pos4), dot(uview.proj[1], mv_pos4), dot(uview.proj[2], mv_pos4), dot(uview.proj[3], mv_pos4));
    gl_Position = clip_pos * vec4(1.0, -1.0, 1.0, 1.0);
    vec4 nrm4 = vec4(in_nrm, 0.0);
    vec3 nrm_tmp = vec3(dot(upush.nrm_mtx[0], nrm4), dot(upush.nrm_mtx[1], nrm4), dot(upush.nrm_mtx[2], nrm4));
    float nrm_len2 = dot(nrm_tmp, nrm_tmp);
    vec3 mv_nrm = nrm_tmp * mix(1.0, inversesqrt(max(nrm_len2, 1e-10)), step(1e-10, nrm_len2));
    vec3 light0_vec = ulights.lights[0].xyz - mv_pos4.xyz;
    float light0_dist2 = dot(light0_vec, light0_vec);
    float light0_dist = sqrt(light0_dist2);
    vec3 light0_dir = light0_vec / light0_dist;
    float cc0_cos0 = clamp(dot(light0_dir, ulights.lights[1].xyz), 0.0, 1.0);
    float cc0_attn0 = max(0.0, dot(ulights.lights[3].xyz, vec3(1.0, cc0_cos0, cc0_cos0 * cc0_cos0)) / dot(ulights.lights[4].xyz, vec3(1.0, light0_dist, light0_dist2)));
    float cc0_diff0 = max(0.0, dot(light0_dir, mv_nrm));
    vec4 cc0_sum0 = uview.channel_colors[0] + cc0_attn0 * cc0_diff0 * ulights.lights[2];
    vec4 cc0_lit = uview.channel_colors[1] * clamp(cc0_sum0, 0.0, 1.0);
    vec4 cc0 = vec4(cc0_lit.rgb, uview.channel_colors[3].a);
    out_cc0 = cc0;
    vec4 tc0 = vec4(in_tex0_uv, 1.0, 1.0);
    vec3 tc0_mtx = vec3(dot(uview.tex_mtx[0], tc0), dot(uview.tex_mtx[1], tc0), dot(uview.tex_mtx[2], tc0));
    vec3 tc0_tmp = vec3(tc0_mtx.xy, 1.0);
    out_tex0_uv = tc0_tmp.xy;
    vec4 tc1 = vec4(in_tex1_uv, 1.0, 1.0);
    vec3 tc1_mtx = vec3(dot(uview.tex_mtx[3], tc1), dot(uview.tex_mtx[4], tc1), dot(uview.tex_mtx[5], tc1));
    vec3 tc1_tmp = vec3(tc1_mtx.xy, 1.0);
    out_tex1_uv = tc1_tmp.xy;
    out_fog_zw = clip_pos.zw;
}
