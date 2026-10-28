#version 450
// PPSSPP's hardware skinning (ppsspp:GPU/Common/VertexShaderGenerator.cpp): each bone scaled by its weight and the
// matrices summed, which glslang makes OpMatrixTimesScalar and a column at a time, then the position and a normal
// through the sum.
layout(std140, set = 0, binding = 0) uniform Bones {
	mat3x4 u_bone0;
	mat3x4 u_bone1;
	mat3x4 u_bone2;
	mat3x4 u_bone3;
	vec4 u_pos;
};
layout(location = 0) in vec4 w1;
layout(location = 0) out vec3 v_normal;

void main()
{
	mat3x4 skin = w1.x * u_bone0 + w1.y * u_bone1 + w1.z * u_bone2 + w1.w * u_bone3;
	vec3 skinned = (vec4(u_pos.xyz, 1.0) * skin).xyz * 1.9921875;
	gl_Position = vec4(skinned, 1.0);
	v_normal = (vec4(u_pos.yzx, 0.0) * skin).xyz;
}
