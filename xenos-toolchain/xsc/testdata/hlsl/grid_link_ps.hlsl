// The grid's link: COLOR0's red and green, and TEXCOORD0's x in blue, read in the other order from the vertex shader's.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    return float4(colour.rg, uv.x, 1.0);
}
