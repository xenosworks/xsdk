// An if on a bool past an unrolled loop, which leaves no hardware loop, so the optimiser would take the program but for
// its execs on a boolean constant, whose arms each write the register the if's value is read from.
float4 Step : register(c0);
bool Flip : register(b0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 0; i < 2; i++)
        c += Step;
    if (Flip)
        c = c.wzyx * 2.0;
    return c;
}
