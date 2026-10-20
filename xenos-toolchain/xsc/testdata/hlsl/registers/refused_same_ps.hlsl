// Two bools in one boolean register, b2, which D3D9 can't set apart: refused.
bool A : register(b2);
bool B : register(b2);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    if (A)
        c = c.yxwz * 2.0;
    if (B)
        c = c * 0.5;
    return c;
}
