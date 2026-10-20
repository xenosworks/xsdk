// A bool in $Global, a float register D3D9 sets 0 or 1, as i17's; a static const bool4 choosing, which spirv-opt folds
// to a shuffle of two vectors, float4s' and float3s'; and a bool4 of constants and a comparison.
bool UseTint : register(c0);
float4 Tint : register(c1);
static const bool4 Mask = bool4(true, false, true, false);

float4 ps_main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR0
{
    float4 c = Mask ? a : b;
    float4 t = UseTint ? Tint : float4(1, 1, 1, 1);
    bool4 m = bool4(true, false, a.x > 0.5, true);
    float3 c3 = Mask.xyz ? a.xyz * 2.0 : b.xyz + 1.0;
    return (m ? c : b) * t + dot(c3, float3(1.0, 2.0, 4.0));
}
