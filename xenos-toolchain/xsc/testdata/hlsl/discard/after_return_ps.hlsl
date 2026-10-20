// A discard after an early return inside a loop: a pixel is killed only if it never returned.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    for (int i = 0; i < 4; i++) {
        c.r += 0.125;
        if (c.r > uv.x)
            return c;
    }
    discard;
    return colour;
}
