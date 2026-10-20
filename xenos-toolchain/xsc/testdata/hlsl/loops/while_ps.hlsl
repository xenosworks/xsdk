// A while the unroller counts, its counter stepped in the pass before a break on data, both read after the loop.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float x = uv.x;
    int i = 0;
    while (i < 6) {
        i++;
        x = x * 1.5 + 0.25;
        if (x > Limit.x)
            break;
    }
    return float4(x, i, uv.y, 1);
}
