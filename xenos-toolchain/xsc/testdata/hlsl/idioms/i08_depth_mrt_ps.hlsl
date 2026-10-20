// Depth output and four colour targets, D3D9's semantics.
sampler2D Albedo : register(s0);
float4 DepthParams : register(c0);

struct PS_OUT {
    float4 Color0 : COLOR0;
    float4 Color1 : COLOR1;
    float4 Color2 : COLOR2;
    float4 Color3 : COLOR3;
    float Depth : DEPTH;
};

PS_OUT ps_main(float2 uv : TEXCOORD0, float3 normal : TEXCOORD1, float depth : TEXCOORD2)
{
    PS_OUT o;
    float4 a = tex2D(Albedo, uv);
    o.Color0 = a;
    o.Color1 = float4(normalize(normal) * 0.5 + 0.5, 1);
    o.Color2 = float4(depth.xxx, 1);
    o.Color3 = float4(uv, 0, 1);
    o.Depth = saturate(depth * DepthParams.x + DepthParams.y);
    return o;
}
