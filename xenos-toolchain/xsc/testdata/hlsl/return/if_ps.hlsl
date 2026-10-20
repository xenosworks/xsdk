// An early return under an if, in a pixel shader without a loop.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5)
        return float4(uv, 0.25, 1.0);
    c.g = uv.y;
    return c;
}
