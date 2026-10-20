// The grid's if/else in a pixel shader without a loop: an if with no else, then nested if/else, each changing a value
// on TEXCOORD0's x.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = float4(colour.rg, uv.x, 1.0);
    if (uv.x > 0.25)
        c.r += 0.5;
    if (uv.x > 0.5) {
        c.g *= 0.5;
        if (uv.x > 0.75)
            c.b = 1.0 - c.b;
        else
            c.r = 0.0;
    } else {
        c.b *= 0.5;
    }
    return c;
}
