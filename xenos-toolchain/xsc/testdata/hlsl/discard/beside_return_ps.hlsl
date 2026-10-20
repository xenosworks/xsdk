// A discard beside an early return: a pixel that returned is never killed.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.75)
        return float4(1.0, 0.0, uv.y, 1.0);
    if (uv.y < 0.5)
        discard;
    c.g = uv.x;
    return c;
}
