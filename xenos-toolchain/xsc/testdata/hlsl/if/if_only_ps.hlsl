// An if without an else that changes a value, in a pixel shader without a loop.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5)
        c.rgb *= 0.5;
    return c;
}
