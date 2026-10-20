// Matrix-palette skinning: float4x3 bones indexed by BLENDINDICES, D3DCOLORtoUBYTE4, BLENDWEIGHT.
float4x4 ViewProj : register(c0);
float4x3 Bones[26] : register(c4);

struct VS_IN {
    float4 pos : POSITION;
    float4 weights : BLENDWEIGHT;
    float4 indices : BLENDINDICES;
    float2 uv : TEXCOORD0;
};
struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VS_OUT vs_main(VS_IN In)
{
    VS_OUT o;
    int4 idx = D3DCOLORtoUBYTE4(In.indices);
    float3 p = mul(In.pos, Bones[idx.x]) * In.weights.x;
    p += mul(In.pos, Bones[idx.y]) * In.weights.y;
    p += mul(In.pos, Bones[idx.z]) * In.weights.z;
    p += mul(In.pos, Bones[idx.w]) * (1.0 - In.weights.x - In.weights.y - In.weights.z);
    o.pos = mul(float4(p, 1), ViewProj);
    o.uv = In.uv;
    return o;
}
