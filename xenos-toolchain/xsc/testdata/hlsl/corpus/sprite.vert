#version 450
// A sprite's corner, already in clip space, with its texture coordinates and colour.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
	gl_Position = vec4(a_position, 0.5, 1.0);
	v_uv = a_uv;
	v_color = a_color;
}
