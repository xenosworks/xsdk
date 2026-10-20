#version 450
// A corner of the triangle: where it is, from -1 to 1 across the screen and up it, and its colour.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 0) out vec3 v_color;

void main()
{
	gl_Position = vec4(a_position, 0.0, 1.0);
	v_color = a_color;
}
