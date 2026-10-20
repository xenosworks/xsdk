// An early return inside a loop an int at register(i0) counts, a hardware loop.
int Count : register(i0);

float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    for (int i = 0; i < Count; i++) {
        c.r += 0.125;
        if (c.r > uv.x)
            return float4(c.r, 0.25, 0.5, 1.0);
    }
    c.g = uv.y;
    return c;
}
