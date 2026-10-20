// A vertex shader with an SV_Position output, which glslang maps to the Position built-in.
float4x4 WorldViewProj : register(c0);
struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS_OUT vs_main(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VS_OUT o;
    o.pos = mul(pos, WorldViewProj);
    o.uv = uv;
    return o;
}
