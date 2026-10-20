// Nested if/else, each arm changing a component of its own.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5) {
        c.r = 1.0 - c.r;
        if (uv.y > 0.5)
            c.g = uv.x;
        else
            c.b = uv.y;
    } else {
        c.a = 0.5;
    }
    return c;
}
