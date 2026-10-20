// Samplers at s1 and s4 and two without a register: D3D9's SetTexture(1) and (4) reach the first two, and the other
// two take the lowest fetch constants no register names, 0 and 2.
sampler2D A : register(s1);
sampler2D B : register(s4);
sampler2D C;
sampler2D D;

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    return float4(tex2D(A, uv).x, tex2D(B, uv).y, tex2D(C, uv).z, tex2D(D, uv).w);
}
