#version 450
// Inputs at locations 1, 3 and 0, declared in that order and skipping 2, as PPSSPP's pixel shaders take a colour, fog
// and a texture coordinate. Each reads its own location's interpolator: 2 takes its register unread.
layout(location = 1) in vec4 v_b;
layout(location = 3) in vec4 v_d;
layout(location = 0) in vec4 v_a;
layout(location = 0) out vec4 o_color;

void main()
{
   o_color = v_a + v_b * 2.0 + v_d * 4.0;
}
