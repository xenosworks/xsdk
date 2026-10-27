#version 450
// gl_VertexIndex in an instanced shader: the prologue's t.x plus the vertex base, the base's register past the group's.
layout(location = 0) in vec4 a_position;

void main()
{
	gl_Position = a_position + vec4(float(gl_VertexIndex), float(gl_InstanceIndex), 0.0, 0.0);
}
