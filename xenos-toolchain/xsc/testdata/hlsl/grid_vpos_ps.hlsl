// The grid's VPOS: the pixel's column and row, each mod 64 and times 4, in red and green, so a pixel off is 4 apart,
// and in blue how far each is from whole.
float4 ps_main(float2 vpos : VPOS) : COLOR0
{
    float2 place = frac(vpos / 64.0) * 64.0;
    float2 apart = frac(vpos);
    return float4(place * (4.0 / 255.0), apart.x + apart.y, 1.0);
}
