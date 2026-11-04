#version 450
// An and with bits that aren't one run, which Xenos's float ALU has no exact form for here: refused.
void main()
{
	gl_Position = vec4(float(gl_VertexIndex & 5), 0.0, 0.0, 1.0);
}
