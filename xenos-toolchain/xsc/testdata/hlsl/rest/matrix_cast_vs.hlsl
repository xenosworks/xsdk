// A (float3x3) cast of a float4x4 in $Global, by rows as glslang lays HLSL's matrices, and a negated vector, as i01's.
float4x4 World : register(c0);
float4x4 WorldViewProj : register(c4);
float4 LightDir : register(c8);

struct VS_OUT {
    float4 pos : POSITION;
    float3 n : TEXCOORD0;
    float4 d : TEXCOORD1;
};

VS_OUT vs_main(float4 pos : POSITION, float3 n : NORMAL)
{
    VS_OUT o;
    o.pos = mul(pos, WorldViewProj);
    o.n = mul(n, (float3x3)World);
    o.d = float4(-LightDir.xyz, dot(o.n, -LightDir.xyz));
    return o;
}
