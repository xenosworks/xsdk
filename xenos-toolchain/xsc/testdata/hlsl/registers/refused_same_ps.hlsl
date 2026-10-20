// Two bools in one boolean register, b2, which D3D9 can't set apart: refused. The loop has the unroller run, so the
// ifs' arms go in execs on boolean constants.
float4 Step : register(c0);
bool A : register(b2);
bool B : register(b2);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 0; i < 2; i++)
        c += Step;
    if (A)
        c = c.yxwz * 2.0;
    if (B)
        c = c * 0.5 + Step;
    return c;
}
