// The grid's link with gaps: COLOR1's red and green, and TEXCOORD3's x in blue, in 2 interpolators, where a map of
// each semantic to one interpolator in both stages would take 14.
float4 ps_main(float4 spec : COLOR1, float4 uv3 : TEXCOORD3) : COLOR0
{
    return float4(spec.rg, uv3.x, 1.0);
}
