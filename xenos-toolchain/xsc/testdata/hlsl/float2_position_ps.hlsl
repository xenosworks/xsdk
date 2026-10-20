// SV_Position declared a float2, the pixel's place used whole.
float4 K : register(c0);

float4 ps_main(float2 pos : SV_Position, float2 uv : TEXCOORD0) : COLOR0
{
    return float4(pos * K.xy + uv, pos);
}
