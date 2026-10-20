// A float3x3 output, TEXCOORD1 to 3 a row each, built of three inputs, for matrix_tbn_ps.hlsl.
struct VS_IN {
    float4 pos : POSITION;
    float3 t : TANGENT;
    float3 b : BINORMAL;
    float3 n : NORMAL;
    float3 v : TEXCOORD0;
};

struct VS_OUT {
    float4 pos : POSITION;
    float3 v : TEXCOORD0;
    float3x3 tbn : TEXCOORD1;
};

VS_OUT vs_main(VS_IN i)
{
    VS_OUT o;
    o.pos = i.pos;
    o.v = i.v;
    o.tbn = float3x3(i.t, i.b, i.n * 2.0);
    return o;
}
