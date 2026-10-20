// A while on data, a hardware loop of 255 passes, whose test works out a value its pass and what follows it read.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float s = 0;
    float k = 1;
    float t;
    while ((t = uv.x * k) < Limit.x) {
        s += t;
        k += 1;
    }
    return float4(s, t, k, 1);
}
