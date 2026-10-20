// A do/while on data, a hardware loop of 255 passes left at its test, and what its pass works out read after it.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float x = uv.x;
    int n = 0;
    do {
        x = x * 1.5 + 0.25;
        n++;
    } while (x < Limit.x);
    return float4(x, n, uv.y, 1);
}
