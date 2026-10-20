// An early return in the else.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5)
        c.rgb *= 0.5;
    else
        return float4(uv, 0.75, 1.0);
    c.a = uv.y;
    return c;
}
