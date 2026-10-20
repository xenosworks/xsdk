// xsdk_test's HLSL grid (xsdk:tests/test_hlsl.c): POSITION, COLOR0 and TEXCOORD0 in, and out with a TEXCOORD1 its pixel
// shaders don't read declared first, so each output's location is another's interpolator.
struct VS_IN {
    float4 pos : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

struct VS_OUT {
    float4 other : TEXCOORD1;
    float2 uv : TEXCOORD0;
    float4 pos : POSITION;
    float4 colour : COLOR0;
};

VS_OUT vs_main(VS_IN i)
{
    VS_OUT o;
    o.other = float4(1.0, 1.0, 0.0, 1.0) - i.colour;
    o.uv = i.uv;
    o.pos = i.pos;
    o.colour = i.colour;
    return o;
}
