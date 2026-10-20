// An unrolled loop: a constant count, an accumulator and the counter read in the pass.
float4 Step : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 s = uv;
    for (int i = 0; i < 5; i++)
        s = s * 0.5 + Step * i;
    return s;
}
