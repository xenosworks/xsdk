// An if whose else alone changes a value.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (uv.x > 0.5) {
    } else {
        c.g = uv.y;
    }
    return c;
}
