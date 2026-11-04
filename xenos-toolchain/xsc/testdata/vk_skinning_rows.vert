#version 450
// The same sum of two bones held as rows, row_major, whose columns are gathered a component a row.
layout(std140, row_major, set = 0, binding = 0) uniform Bones {
	mat3x4 u_bone0;
	mat3x4 u_bone1;
	vec4 u_pos;
};
layout(location = 0) in vec4 w1;

void main()
{
	mat3x4 skin = w1.x * u_bone0 + w1.y * u_bone1;
	gl_Position = vec4((vec4(u_pos.xyz, 1.0) * skin).xyz, 1.0);
}
