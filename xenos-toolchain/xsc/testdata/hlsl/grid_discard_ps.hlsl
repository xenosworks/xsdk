// xsdk_test's discard cell: a clip before an if, and a discard in an if inside another, on TEXCOORD0's x, u. Columns
// left of an eighth are clipped and those between a half and three quarters discarded; the others draw COLOR0's red
// and green and u, changed by the ifs.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = float4(colour.rg, uv.x, 1.0);
    clip(uv.x - 0.125);
    if (uv.x > 0.5) {
        c.r += 0.5;
        if (uv.x < 0.75)
            discard;
        c.b = 1.0 - c.b;
    }
    c.g *= 0.5;
    return c;
}
