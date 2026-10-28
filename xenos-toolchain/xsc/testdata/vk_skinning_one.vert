#version 450
// One bone, as PPSSPP skins with one weight: the bone by its weight alone, kept in a local.
layout(std140, set = 0, binding = 0) uniform Bones {
	mat3x4 u_bone0;
	vec4 u_pos;
};
layout(location = 0) in vec4 w1;

void main()
{
	mat3x4 skin = w1.x * u_bone0;
	gl_Position = vec4((vec4(u_pos.xyz, 1.0) * skin).xyz, 1.0);
}
