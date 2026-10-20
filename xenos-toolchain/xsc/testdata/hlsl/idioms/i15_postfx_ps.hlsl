// A post-process pass: tone map with exp/log, vignette, a static const offset table read in an unrolled loop,
// a helper with an out parameter, and a ternary.
sampler2D Scene : register(s0);
sampler2D Bloom : register(s1);
float4 Exposure : register(c0);
static const float2 Offsets[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };

void ToneMap(float3 hdr, out float3 ldr)
{
    ldr = 1 - exp(-hdr * Exposure.x);
}

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 bloom = 0;
    for (int i = 0; i < 4; i++)
        bloom += tex2D(Bloom, uv + Offsets[i] * Exposure.zw).rgb;
    float3 hdr = tex2D(Scene, uv).rgb + bloom * 0.25;
    float3 ldr;
    ToneMap(hdr, ldr);
    float2 d = uv - 0.5;
    float vig = 1 - dot(d, d) * Exposure.y;
    ldr = vig > 0 ? ldr * vig : 0;
    return float4(log2(ldr + 1), 1);
}
