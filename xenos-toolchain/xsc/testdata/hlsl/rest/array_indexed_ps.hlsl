// Static const arrays at run-time indices: a float4's at a hardware loop's counter, an int in $Global counting it, and
// at an index worked out from TEXCOORD0, which xost reads at a0 from literal registers of their own, a float's in
// every component.
float4 Tint : register(c0);
int Count : register(c1);
static const float4 Colours[6] = { float4(0.5, 0.25, 0.125, 1), float4(-1, 2, 0.75, 0.5), float4(4, -0.5, 1, 0.25),
    float4(0.0625, 8, -2, 1), float4(3, 1.5, 0.5, -1), float4(-0.25, -4, 6, 2) };
static const float Weights[6] = { 1, 0.5, 2, -1, 0.25, 4 };

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = 0;
    for (int i = 0; i < Count; i++)
        c += Colours[i] * Weights[i] + Tint * uv.x;
    int k = (int)(uv.y * 4.0);
    return c + Colours[k] + Weights[k];
}
