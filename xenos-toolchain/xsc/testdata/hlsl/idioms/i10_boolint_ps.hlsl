// D3D9's boolean and integer constants: bool in b#, int in i#, a loop on an int constant (static flow control).
sampler2D Tex : register(s0);
bool UseTexture : register(b0);
int LightCount : register(i0);
float4 LightColor[4] : register(c0);
float4 LightDir[4] : register(c4);

float4 ps_main(float2 uv : TEXCOORD0, float3 n : TEXCOORD1) : COLOR0
{
    float3 c = 0;
    for (int i = 0; i < LightCount; i++)
        c += LightColor[i].rgb * saturate(dot(normalize(n), LightDir[i].xyz));
    float4 base = UseTexture ? tex2D(Tex, uv) : float4(1, 1, 1, 1);
    return float4(c, 1) * base;
}
