// xsdk_test's HLSL grid's skinning (xsdk:tests/test_hlsl.c): each corner of a quad skinned by four bones of a float4x3
// palette, 3 registers a bone, its int4 BLENDINDICES picks, UBYTE4s, weighted by BLENDWEIGHT's x to z and what they
// leave; out the skinned x and y, the skinned z as COLOR0's red, COLOR0's green 0.75 and TEXCOORD0 as it came.
float4x3 Bones[8] : register(c0);

struct VS_IN {
    float4 pos : POSITION;
    float4 weights : BLENDWEIGHT;
    int4 indices : BLENDINDICES;
    float2 uv : TEXCOORD0;
};

struct VS_OUT {
    float4 pos : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

VS_OUT vs_main(VS_IN i)
{
    float3 p = mul(i.pos, Bones[i.indices.x]) * i.weights.x;
    p += mul(i.pos, Bones[i.indices.y]) * i.weights.y;
    p += mul(i.pos, Bones[i.indices.z]) * i.weights.z;
    p += mul(i.pos, Bones[i.indices.w]) * (1.0 - i.weights.x - i.weights.y - i.weights.z);
    VS_OUT o;
    o.pos = float4(p.xy, 0.5, 1.0);
    o.colour = float4(p.z, 0.75, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}
