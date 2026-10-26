#version 450
// Derivatives of a vec2, a vec3 and a vec4: getGradients takes two components a fetch, so the vec3 and the vec4 each
// take two, their xy and their zw (src/xsc_lower_fragment.c, derivative_pair). RetroArch's XMB ribbon takes dFdx and
// dFdy of a vec3 and lit nothing on the console while its z came from the first fetch.
layout(location = 0) in vec2 v_a;
layout(location = 1) in vec3 v_b;
layout(location = 2) in vec4 v_c;
layout(location = 0) out vec4 o_color;

void main()
{
   vec2 da = dFdy(v_a);
   vec3 db = dFdx(v_b);
   vec4 dc = fwidth(v_c);
   o_color = vec4(da, 0.0, 0.0) + vec4(db, 0.0) + dc;
}
