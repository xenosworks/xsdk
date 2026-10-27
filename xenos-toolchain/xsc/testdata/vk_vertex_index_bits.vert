#version 450
// gl_VertexIndex's bits: shifts by constants, and ands with runs of bits from bit 0, from past it, to bit 31, every bit
// and none.
layout(location = 0) out vec4 v_bits;
layout(location = 1) out vec4 v_more;

void main()
{
	int i = gl_VertexIndex;
	v_bits = vec4(float(i >> 2), float(i & 0xF0), float(i & ~3), float((i & 7) << 3));
	v_more = vec4(float(i & -1), float(i & 0), float(3 & i), float(i >> 0));
	gl_Position = vec4(float(i), 0.0, 0.0, 1.0);
}
