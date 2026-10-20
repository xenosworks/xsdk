// Refused: a value a loop's test works out, t, kept past the loop where a break sets it too.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float s = 0;
    float k = 1;
    float t;
    while ((t = uv.x * k) < Limit.x) {
        s += t;
        k += 1;
        if (s > Limit.y) {
            t = -1;
            break;
        }
    }
    return float4(s, t, k, 1);
}
