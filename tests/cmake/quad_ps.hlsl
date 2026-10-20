// cmake_check's binding as D3D9 binds: Ramp at s2, Swap at b1 an if reads, Count at i3 a loop counts and Step at c3.
// Red and green are Ramp's texel's, swapped where Swap is set, and blue a step for each pass.
sampler2D Ramp : register(s2);
bool Swap : register(b1);
int Count : register(i3);
float4 Step : register(c3);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = tex2D(Ramp, uv);
    for (int i = 0; i < Count; i++)
        c.b += Step.x;
    if (Swap)
        c.rg = c.gr;
    return c;
}
