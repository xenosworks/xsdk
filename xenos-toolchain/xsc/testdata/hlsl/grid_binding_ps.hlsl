// The grid's binding as D3D9 binds: samplers at s3 and s7, a bool at b2 an if reads and an int at i1 a loop counts,
// each found by name in the container's table; red and green from the two textures, blue the passes and the arm.
sampler2D Low : register(s3);
sampler2D High : register(s7);
bool Bright : register(b2);
int Count : register(i1);
float4 Step : register(c2);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = float4(tex2D(Low, uv).r, tex2D(High, uv).g, 0.0, 1.0);
    for (int i = 0; i < Count; i++)
        c.b += Step.x;
    if (Bright)
        c.b += Step.y;
    return c;
}
