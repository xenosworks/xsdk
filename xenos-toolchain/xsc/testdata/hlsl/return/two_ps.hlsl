// Two early returns, a value changed between them.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.75)
        return float4(1.0, 0.0, 0.25, 1.0);
    c.g = uv.y;
    if (uv.y > 0.5)
        return c * 0.5;
    c.b = uv.x;
    return c;
}
