// An early return in a switch's case, which spirv-opt makes a flag set and a break, then a return after the switch.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    switch ((int)(uv.x * 4.0)) {
    case 0:
        return float4(1.0, 0.0, 0.25, 1.0);
    case 1:
        c.g = uv.y;
        break;
    default:
        c.b = 0.75;
        break;
    }
    c.a = 0.5;
    return c;
}
