// sampler2D / samplerCUBE / sampler3D, tex2D, texCUBE, tex2Dlod, tex2Dbias, tex2Dproj, tex3D, register(s#).
sampler2D DiffuseMap : register(s0);
sampler2D DetailMap : register(s1);
samplerCUBE EnvMap : register(s2);
sampler3D VolumeMap : register(s3);
sampler2D ProjMap : register(s4);
float4 Params : register(c0);

float4 ps_main(float2 uv : TEXCOORD0, float3 refl : TEXCOORD1, float4 proj : TEXCOORD2, float3 vol : TEXCOORD3) : COLOR
{
    float4 base = tex2D(DiffuseMap, uv);
    float4 detail = tex2Dbias(DetailMap, float4(uv * 8.0, 0, Params.x));
    float4 lod = tex2Dlod(DiffuseMap, float4(uv, 0, Params.y));
    float4 env = texCUBE(EnvMap, refl);
    float4 v = tex3D(VolumeMap, vol);
    float4 p = tex2Dproj(ProjMap, proj);
    return base * detail + lod * 0.25 + env * Params.z + v * p;
}
