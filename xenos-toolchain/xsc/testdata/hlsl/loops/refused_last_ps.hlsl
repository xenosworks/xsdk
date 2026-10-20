// Refused: a do/while's header's Phi, x as a pass began, read after the loop, where its variable holds the next pass's.
float4 Limit : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float x = uv.x;
    float last;
    do {
        last = x;
        x = x * 2 + 0.25;
    } while (x < Limit.x);
    return float4(last, x, 0, 1);
}
