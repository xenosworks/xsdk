// D3D9's vertex semantics among a struct's members, declared out of order: each input's usage and index from its
// semantic, POSITION and PSIZE as the position and the point's size, and the rest at the interpolators their
// semantics link at.
float4 Offset : register(c0);

struct VS_IN {
    float4 normal : NORMAL;
    float4 pos : POSITION;
    float4 weights : BLENDWEIGHT;
    float4 indices : BLENDINDICES;
    float4 tangent : TANGENT;
    float4 binormal : BINORMAL;
    float4 colour : COLOR1;
    float4 uv : TEXCOORD3;
};

struct VS_OUT {
    float4 colour : COLOR0;
    float fog : FOG;
    float4 pos : POSITION;
    float size : PSIZE;
    float4 uv1 : TEXCOORD1;
    float4 uv0 : TEXCOORD0;
    float4 spec : COLOR1;
};

VS_OUT vs_main(VS_IN i)
{
    VS_OUT o;
    o.colour = i.colour * 2.0;
    o.fog = i.weights.x;
    o.pos = i.pos + Offset;
    o.size = i.indices.y + 1.0;
    o.uv1 = i.normal + i.tangent;
    o.uv0 = i.uv;
    o.spec = i.binormal - i.weights;
    return o;
}
