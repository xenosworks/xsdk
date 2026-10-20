// A typical SM3 per-pixel light: normal map, Blinn-Phong with pow, three lights in a constant loop, helper functions.
sampler2D DiffuseMap : register(s0);
sampler2D NormalMap : register(s1);
float3 LightPos[3] : register(c0);
float4 LightColor[3] : register(c3);
float4 EyePos : register(c6);
float4 Specular : register(c7);

float3 Unpack(float4 n) { return n.xyz * 2 - 1; }

float3 Shade(float3 N, float3 V, float3 P, int i)
{
    float3 L = normalize(LightPos[i] - P);
    float3 H = normalize(L + V);
    float d = saturate(dot(N, L));
    float s = pow(saturate(dot(N, H)), Specular.w);
    return LightColor[i].rgb * (d + s * Specular.rgb);
}

float4 ps_main(float2 uv : TEXCOORD0, float3 worldPos : TEXCOORD1, float3x3 tbn : TEXCOORD2) : COLOR0
{
    float3 N = normalize(mul(Unpack(tex2D(NormalMap, uv)), tbn));
    float3 V = normalize(EyePos.xyz - worldPos);
    float3 light = 0;
    for (int i = 0; i < 3; i++)
        light += Shade(N, V, worldPos, i);
    return float4(tex2D(DiffuseMap, uv).rgb * light, 1);
}
