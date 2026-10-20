// The grid's early returns in a pixel shader without a loop, on TEXCOORD0's x: one under an if and one in an if inside
// another, values changed before, between and after them.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = float4(colour.rg, uv.x, 1.0);
    if (uv.x > 0.75)
        return float4(1.0, 0.0, 1.0 - uv.x, 1.0);
    c.r += 0.5;
    if (uv.x > 0.25) {
        c.g *= 0.5;
        if (uv.x < 0.5)
            return c;
        c.b = 1.0 - c.b;
    }
    c.r *= 0.5;
    return c;
}
