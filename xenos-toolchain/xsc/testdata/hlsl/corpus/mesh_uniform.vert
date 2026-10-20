#version 450
// A cube placed by the matrix in a dynamic uniform buffer, as encounter's Aurora passes its uniforms.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(set = 1, binding = 0) uniform Placement { mat4 mvp; } u_placement;

void main()
{
	gl_Position = u_placement.mvp * vec4(a_position, 1.0);
	v_uv = a_uv;
}
