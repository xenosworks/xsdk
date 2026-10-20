// An early return inside a loop, which spirv-opt makes a flag set and a break, then a return after the loop where the
// flag is set; the loop unrolled.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    for (int i = 0; i < 4; i++) {
        c.r += 0.125;
        if (c.r > uv.x)
            return float4(c.r, i * 0.25, 0.5, 1.0);
    }
    c.g = uv.y;
    return c;
}
