// An alpha test, a clip on a sample's alpha, then an if and a second sample at a coordinate worked out after it,
// whose LOD the quad's killed pixels help make.
sampler2D Tex : register(s1);

float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 t = tex2D(Tex, uv);
    clip(t.a - 0.5);
    float2 p = uv * 2.0;
    if (uv.x > 0.5)
        t.rgb *= colour.rgb;
    return tex2D(Tex, p) * t;
}
