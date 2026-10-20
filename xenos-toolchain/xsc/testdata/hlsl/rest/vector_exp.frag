#version 450
// vector_exp_ps.hlsl's twin.
layout(location = 0) in vec4 a;
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(exp(a.xyz).xy, log2(a.zw + 2.0)) + vec4(sqrt(a.yw * a.yw), sin(a.xz));
}
