// D3D9's VPOS, the pixel's whole column and row, and VFACE, positive on a front face and negative on a back one,
// beside a TEXCOORD0.
float4 ps_main(float2 uv : TEXCOORD0, float2 vpos : VPOS, float face : VFACE) : COLOR0
{
    return float4(vpos, face, uv.x);
}
