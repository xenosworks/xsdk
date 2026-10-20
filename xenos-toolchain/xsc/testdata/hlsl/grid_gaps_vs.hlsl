// xsdk_test's HLSL grid (xsdk:tests/test_hlsl.c), its link with gaps: outputs TEXCOORD3, COLOR1, TEXCOORD0 and FOG,
// interpolators 1, 2, 0 and 3 in this stage, of which grid_gaps_ps.hlsl reads COLOR1 and TEXCOORD3, its 1 and 0.
struct VS_IN {
    float4 pos : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

struct VS_OUT {
    float4 uv3 : TEXCOORD3;
    float4 spec : COLOR1;
    float4 uv0 : TEXCOORD0;
    float4 pos : POSITION;
    float fog : FOG;
};

VS_OUT vs_main(VS_IN i)
{
    VS_OUT o;
    o.uv3 = float4(i.uv, 0.0, 1.0);
    o.spec = i.colour;
    o.uv0 = float4(1.0, 1.0, 0.0, 1.0) - i.colour;
    o.pos = i.pos;
    o.fog = 0.5 - i.uv.x;
    return o;
}
