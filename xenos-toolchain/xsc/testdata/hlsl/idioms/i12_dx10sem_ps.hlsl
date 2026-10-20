// The same pixel work in DX10 semantics glslang maps to built-ins: SV_Position, SV_IsFrontFace, SV_Target, SV_Depth.
sampler2D Tex : register(s0);
struct PS_OUT { float4 c0 : SV_Target0; float d : SV_Depth; };
PS_OUT ps_main(float4 pos : SV_Position, float2 uv : TEXCOORD0, bool front : SV_IsFrontFace)
{
    PS_OUT o;
    o.c0 = tex2D(Tex, uv) * (front ? 1.0 : 0.5);
    o.c0.rg += frac(pos.xy * 0.125);
    o.d = 0.25;
    return o;
}
