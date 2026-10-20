#version 450
// A corner of a face: where it sits on the cube, its face's colour, and where it sits in the texture.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_color;
// The matrix that turns the cube and puts it in perspective, which the title sets every frame.
layout(push_constant) uniform Push { mat4 mvp; } pc;

void main()
{
	gl_Position = pc.mvp * vec4(a_position, 1.0);
	v_uv = a_uv;
	v_color = a_color;
}
