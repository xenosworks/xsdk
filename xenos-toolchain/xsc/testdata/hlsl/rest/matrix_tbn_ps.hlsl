// A float3x3 interpolator, TEXCOORD1 to 3 a row each, as i11's tangent frame.
float4 ps_main(float3 v : TEXCOORD0, float3x3 tbn : TEXCOORD1) : COLOR0
{
    return float4(mul(v, tbn), 1.0);
}
