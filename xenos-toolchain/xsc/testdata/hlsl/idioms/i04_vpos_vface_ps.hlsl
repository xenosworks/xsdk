// VPOS and VFACE as D3D9 has them: VPOS the integer pixel position, the sign of VFACE the facing.
float4 ScreenSize : register(c0);

float4 ps_main(float2 uv : TEXCOORD0, float2 vpos : VPOS, float face : VFACE) : COLOR0
{
    float2 screen = vpos * ScreenSize.zw;
    float3 n = float3(0, 0, 1) * face;
    return float4(screen, n.z * 0.5 + 0.5, 1);
}
