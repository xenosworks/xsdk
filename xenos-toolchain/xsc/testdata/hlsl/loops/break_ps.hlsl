// An unrolled loop left on data: the sum and the counter read after it.
float4 Threshold : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float s = 0;
    int i;
    for (i = 0; i < 8; i++) {
        s += uv.x + i * 0.125;
        if (s > Threshold.x)
            break;
    }
    return float4(s, i, uv.y, 1);
}
