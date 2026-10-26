#version 450
// clamp between a block's values, as PPSSPP clamps texture coordinates to u_texclamp, and between constants that fold
// (tests/test_clamp_bounds.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform Push { vec4 a; vec4 b; } pc;

void main()
{
   o_color = vec4(clamp(v_uv, pc.a.xy, pc.a.zw), clamp(v_uv.x, 0.2, 0.4), clamp(v_uv.y, 0.0, 1.0));
}
