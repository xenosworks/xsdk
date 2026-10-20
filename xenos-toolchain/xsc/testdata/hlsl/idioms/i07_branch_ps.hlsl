// Static branches (on constants), an [unroll]ed constant loop, a static const array of weights.
sampler2D Tex : register(s0);
float4 TexelSize : register(c0);
float4 Mode : register(c1);
static const float Weights[5] = { 0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162 };

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 sum = tex2D(Tex, uv) * Weights[0];
    [unroll] for (int i = 1; i < 5; i++) {
        float2 o = float2(TexelSize.x * i, 0);
        sum += tex2D(Tex, uv + o) * Weights[i];
        sum += tex2D(Tex, uv - o) * Weights[i];
    }
    if (Mode.x > 0.5)
        sum.rgb = dot(sum.rgb, float3(0.3, 0.59, 0.11)).xxx;
    else
        sum.rgb *= Mode.yzw;
    return sum;
}
