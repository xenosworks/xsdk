// Globals, default (column_major) float4x4, mul(v, M), D3D9 input/output semantics, out struct.
float4x4 WorldViewProj : register(c0);
float4x4 World : register(c4);
float4 LightDir : register(c8);
float4 MaterialColor : register(c9);

struct VS_INPUT {
    float4 Position : POSITION;
    float3 Normal : NORMAL;
    float2 Tex0 : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VS_OUTPUT {
    float4 Position : POSITION;
    float2 Tex0 : TEXCOORD0;
    float4 Diffuse : COLOR0;
};

VS_OUTPUT vs_main(VS_INPUT In)
{
    VS_OUTPUT Out;
    Out.Position = mul(In.Position, WorldViewProj);
    float3 n = normalize(mul(In.Normal, (float3x3)World));
    float ndl = saturate(dot(n, -LightDir.xyz));
    Out.Diffuse = In.Color * MaterialColor * ndl;
    Out.Tex0 = In.Tex0;
    return Out;
}
