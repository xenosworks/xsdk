// An if on a texture sample's alpha, changing the sample.
sampler2D Tex : register(s1);
float4 Scale : register(c2);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = tex2D(Tex, uv);
    if (c.a < Scale.w)
        c.rgb *= Scale.rgb;
    return c;
}
