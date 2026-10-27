#version 450
// gl_VertexIndex beside an input: the input fetched at r0.x into r1, gl_VertexIndex r0.x plus the vertex base.
layout(location = 0) in vec4 a_position;

void main()
{
	gl_Position = a_position + vec4(float(gl_VertexIndex), 0.0, 0.0, 0.0);
}
