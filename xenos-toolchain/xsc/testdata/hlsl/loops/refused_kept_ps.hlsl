// Refused: a value a do/while's pass works out, t, read after the loop, which no Phi of its header carries.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float x = uv.x;
    float t;
    do {
        t = x * 0.5;
        x = x * 1.5 + 0.25;
    } while (x < Limit.x);
    return float4(x, t, 0, 1);
}
