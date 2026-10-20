// A discard in an if inside another, values changed before and after it.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5) {
        c.r = 1.0 - c.r;
        if (uv.y < 0.5)
            discard;
        c.b = uv.y;
    }
    c.g = uv.x;
    return c;
}
