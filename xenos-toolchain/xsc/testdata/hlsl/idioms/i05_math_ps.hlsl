// clip(), ddx/ddy, lerp, saturate, frac, fmod, step, smoothstep, pow, abs, sign.
sampler2D Tex : register(s0);
float4 Threshold : register(c0);
float4 ColorA : register(c1);
float4 ColorB : register(c2);

float4 ps_main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
    float4 t = tex2D(Tex, uv);
    clip(t.a - Threshold.x);
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    float edge = saturate(length(dx) + length(dy));
    float stripes = frac(uv.x * 10.0);
    float m = fmod(uv.y * 7.0, 2.0);
    float s = smoothstep(0.2, 0.8, stripes) * step(1.0, m);
    float4 c = lerp(ColorA, ColorB, s) * col;
    c.rgb = pow(abs(c.rgb), 2.2) * sign(t.r - 0.5) + edge;
    return saturate(c);
}
