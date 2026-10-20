// exp, log2, sqrt and sin of vectors, a component each in the scalar unit, as i15's tone map.
float4 ps_main(float4 a : TEXCOORD0) : COLOR0
{
    return float4(exp(a.xyz).xy, log2(a.zw + 2.0)) + float4(sqrt(a.yw * a.yw), sin(a.xz));
}
