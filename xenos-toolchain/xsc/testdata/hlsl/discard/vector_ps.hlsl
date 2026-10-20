// A clip on a float4, which discards where any component is below 0, then an if.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    clip(float4(uv.x - 0.25, uv.y - 0.25, 0.75 - uv.x, 0.75 - uv.y));
    if (uv.x > 0.5)
        c.r = uv.y;
    return c;
}
