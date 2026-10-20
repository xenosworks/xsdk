// A discard in an unrolled loop, beside a break: a pixel is killed at the first pass whose red passes its x, unless
// that red passed its y first.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    for (int i = 0; i < 4; i++) {
        c.r += 0.125;
        if (c.r > uv.y)
            break;
        if (c.r > uv.x)
            discard;
    }
    c.g = uv.y;
    return c;
}
