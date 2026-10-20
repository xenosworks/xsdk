// Two inputs of one semantic.
float4 ps_main(float2 a : TEXCOORD0, float2 b : TEXCOORD0) : COLOR0
{
    return float4(a, b);
}
