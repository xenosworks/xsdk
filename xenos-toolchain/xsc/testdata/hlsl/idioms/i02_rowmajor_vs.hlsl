// row_major matrices, mul(M, v), float4x3 world, float3x3 normal matrix, FOG and PSIZE outputs.
row_major float4x4 ViewProj : register(c0);
row_major float4x3 World : register(c4);
float3x3 NormalMatrix : register(c8);
float4 FogParams : register(c11);

struct VS_OUT {
    float4 pos : POSITION;
    float3 normal : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float fog : FOG;
    float psize : PSIZE;
};

VS_OUT vs_main(float4 pos : POSITION, float3 normal : NORMAL)
{
    VS_OUT o;
    float3 wp = mul(pos, World);
    o.pos = mul(ViewProj, float4(wp, 1));
    o.normal = mul(NormalMatrix, normal);
    o.worldPos = wp;
    o.fog = saturate((FogParams.y - o.pos.z) * FogParams.x);
    o.psize = 4.0;
    return o;
}
