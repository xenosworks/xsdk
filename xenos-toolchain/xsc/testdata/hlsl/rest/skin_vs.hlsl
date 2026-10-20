// i09's skinning: D3DCOLORtoUBYTE4 of a float4 BLENDINDICES, an int4 picking float4x3 bones by their 3 rows each.
float4x3 Bones[8] : register(c0);

struct VS_IN {
    float4 pos : POSITION;
    float4 weights : BLENDWEIGHT;
    float4 indices : BLENDINDICES;
};

float4 vs_main(VS_IN i) : POSITION
{
    int4 idx = D3DCOLORtoUBYTE4(i.indices);
    float3 p = mul(i.pos, Bones[idx.x]) * i.weights.x;
    p += mul(i.pos, Bones[idx.y]) * i.weights.y;
    p += mul(i.pos, Bones[idx.z]) * i.weights.z;
    p += mul(i.pos, Bones[idx.w]) * (1.0 - i.weights.x - i.weights.y - i.weights.z);
    return float4(p, 1.0);
}
