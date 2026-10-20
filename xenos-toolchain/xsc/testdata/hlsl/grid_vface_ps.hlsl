// The grid's VFACE: its sign in red, 255 for +1 and 0 for -1, its size in green and a quarter in blue.
float4 ps_main(float face : VFACE) : COLOR0
{
    return float4(face * 0.5 + 0.5, abs(face), 0.25, 1.0);
}
