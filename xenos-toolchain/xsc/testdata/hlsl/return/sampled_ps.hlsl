// An early return after a texture sample, on its alpha.
sampler2D Tex : register(s1);
float4 Tint : register(c2);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 t = tex2D(Tex, uv);
    if (t.a < Tint.w)
        return t * Tint;
    t.rgb = t.rgb * 0.5 + uv.x;
    return t;
}
