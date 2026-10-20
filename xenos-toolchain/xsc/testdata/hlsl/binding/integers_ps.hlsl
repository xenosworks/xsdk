// Bools and ints out of the float registers glslang packs them into: Flip at b3 an if reads, a boolean constant; Count
// at i2 a loop counts, a loop constant; Tint at b1 and Scale at i0 read as values, each a float register of its own,
// as glslang puts all four in c2 beside each other.
float4 Colours[2] : register(c0);
bool Flip : register(b3);
bool Tint : register(b1);
int Count : register(i2);
int Scale : register(i0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 0; i < Count; i++)
        c += Colours[0];
    if (Flip)
        c = c.wzyx * 2.0;
    c += Tint ? Colours[1] : float4(0.0, 0.0, 0.0, 0.0);
    return c * Scale;
}
