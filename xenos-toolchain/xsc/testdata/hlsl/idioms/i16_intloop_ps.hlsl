// A loop on an int constant alone, the int in a float register of its own (no b# or i#).
float4 LightColor[4] : register(c0);
float4 LightDir[4] : register(c4);
int LightCount : register(c8);

float4 ps_main(float3 n : TEXCOORD0) : COLOR0
{
    float3 c = 0;
    for (int i = 0; i < LightCount; i++)
        c += LightColor[i].rgb * saturate(dot(normalize(n), LightDir[i].xyz));
    return float4(c, 1);
}
