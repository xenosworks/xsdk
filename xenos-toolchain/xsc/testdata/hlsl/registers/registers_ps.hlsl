// DX9's b# and i#: Low at b0 is boolean constant 128, and Tint at b1, read as a value, keeps 129 for a title setting
// b1, so Plain, without a register and the first if's, takes 130; Outer at i0 is loop constant 16, so Inner, without a
// register and the first loop's, takes 17.
float4 Step : register(c0);
bool Plain;
bool Low : register(b0);
bool Tint : register(b1);
int Inner;
int Outer : register(i0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 0; i < Inner; i++)
        c.x += Step.w;
    for (int j = 0; j < Outer; j++)
        c += Step;
    if (Plain)
        c = c.yxwz * 2.0;
    if (Low)
        c = c * 0.5 + Step;
    return c + (Tint ? Step : float4(0.0, 0.0, 0.0, 0.0));
}
