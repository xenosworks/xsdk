// A switch on an int from TEXCOORD0's x: cases that break, two literals sharing one, and a default.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    switch ((int)(uv.x * 8.0)) {
    case 0:
        c.r = 0.25;
        break;
    case 2:
    case 3:
        c.g = uv.y;
        break;
    case 5:
        c.b = 0.75;
        c.a = 0.5;
        break;
    default:
        c *= 0.5;
        break;
    }
    return c;
}
