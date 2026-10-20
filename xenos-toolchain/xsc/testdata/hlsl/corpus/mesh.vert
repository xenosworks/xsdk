#version 450
// A cube placed by the matrix in the vertex range of the push constants.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(push_constant) uniform Push { mat4 mvp; } pc;

void main()
{
	gl_Position = pc.mvp * vec4(a_position, 1.0);
	v_uv = a_uv;
}
