// Constants in a cbuffer with packoffset, as SM3-era code that also targets DX10 writes them.
sampler2D Tex : register(s0);
cbuffer PerMaterial : register(b0) {
    float4 Diffuse : packoffset(c0);
    float3 Emissive : packoffset(c1);
    float Alpha : packoffset(c1.w);
};
float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 t = tex2D(Tex, uv);
    return float4(t.rgb * Diffuse.rgb + Emissive, t.a * Alpha);
}
