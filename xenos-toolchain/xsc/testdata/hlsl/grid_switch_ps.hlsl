// The grid's switch in a pixel shader, on an int from TEXCOORD0's x: cases that break, two literals sharing one, and a
// default.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = float4(colour.rg, uv.x, 1.0);
    switch ((int)(uv.x * 5.0)) {
    case 0:
        c.r = 1.0;
        break;
    case 1:
    case 3:
        c.g *= 0.5;
        c.b = 1.0 - c.b;
        break;
    case 2:
        c.rg = c.gr;
        break;
    default:
        c.b *= 0.5;
        break;
    }
    return c;
}
