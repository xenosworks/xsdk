// VPOS declared a float4, as SPIRV-Cross writes gl_FragCoord's: its z and w, past D3D9's two, read 0.
float4 ps_main(float4 vpos : VPOS) : COLOR0
{
    return vpos.zwxy;
}
