#version 450
// A quad from gl_VertexIndex alone, as PPSSPP's stencil upload draws one (ppsspp:GPU/Common/StencilCommon.cpp,
// GenerateStencilVs).
layout(location = 0) out vec2 v_texcoord;

void main()
{
	float x = float((gl_VertexIndex & 1) << 1);
	float y = float(gl_VertexIndex & 2);
	v_texcoord = vec2(x, y);
	gl_Position = vec4(v_texcoord * 2.0 - vec2(1.0, 1.0), 0.0, 1.0);
}
