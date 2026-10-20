// Static const arrays read at an unrolled loop's counter, a float's and a float2's, as i07's weights and i15's offsets.
float4 Scale : register(c0);
static const float Weights[5] = { 0.5, 0.25, 0.125, 2.0, -1.0 };
static const float2 Offsets[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 sum = uv * Weights[0];
    [unroll] for (int i = 1; i < 5; i++)
        sum += uv.yzwx * Weights[i] * Scale.x;
    float2 o = 0;
    for (int j = 0; j < 4; j++)
        o += Offsets[j] * uv.xy * (j + 1);
    return float4(sum.xy + o, sum.zw);
}
