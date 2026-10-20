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
layout(push_constant, std140) uniform Push {
    vec4 pos_mtx[3];
} upush;
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_clr0;
layout(location = 0) out vec4 out_cc0;
void main() {
    vec4 pos4 = vec4(in_pos, 1.0);
    vec4 mv_pos4 = vec4(vec3(dot(upush.pos_mtx[0], pos4), dot(upush.pos_mtx[1], pos4), dot(upush.pos_mtx[2], pos4)), 1.0);
    vec4 clip_pos = vec4(dot(uview.proj[0], mv_pos4), dot(uview.proj[1], mv_pos4), dot(uview.proj[2], mv_pos4), dot(uview.proj[3], mv_pos4));
    gl_Position = clip_pos * vec4(1.0, -1.0, 1.0, 1.0);
    vec4 cc0 = vec4(uview.channel_colors[1].rgb, in_clr0.a);
    out_cc0 = cc0;
}
