// cmake_check's quad: its place in clip space, and TEXCOORD0 across it for quad_ps.hlsl.
struct VS_OUT {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUT vs_main(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VS_OUT o;
    o.pos = pos;
    o.uv = uv;
    return o;
}
