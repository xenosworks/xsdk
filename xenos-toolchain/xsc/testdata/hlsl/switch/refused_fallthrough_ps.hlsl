// A case falling through to the next, which xost refuses.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    switch ((int)(uv.x * 4.0)) {
    case 0:
        c.r = 0.25;
    case 1:
        c.g = 0.5;
        break;
    default:
        c.a = 0.5;
        break;
    }
    return c;
}
