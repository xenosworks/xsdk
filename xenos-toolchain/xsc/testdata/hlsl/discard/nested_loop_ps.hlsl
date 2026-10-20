// A discard in an unrolled loop inside another, past the outer loop's break on data: a pixel that left the outer loop
// isn't killed by the inner one.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    for (int i = 0; i < 3; i++) {
        c.r += 0.125;
        if (c.r > uv.y)
            break;
        for (int j = 0; j < 2; j++) {
            c.b += 0.125;
            if (c.b > uv.x + 1.0)
                discard;
        }
    }
    return c;
}
