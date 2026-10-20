// An if changing a value, then a clip on the value it changed.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5)
        c.g = uv.y;
    clip(c.g - 0.5);
    c.b = uv.x;
    return c;
}
