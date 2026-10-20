// An unrolled loop whose pass a continue on data ends early.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float s = 0;
    for (int i = 0; i < 6; i++) {
        float v = uv.x * i;
        if (v > Limit.x)
            continue;
        s += v;
    }
    return float4(s, uv.y, uv.z, uv.w);
}
