// An int in a boolean register, b0, where D3D9 keeps bools: refused.
float4 Step : register(c0);
int Count : register(b0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 0; i < Count; i++)
        c += Step;
    return c;
}
