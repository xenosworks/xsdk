// The constant table's entries as D3D9 names them: a matrix held by columns and one by rows, a vector of 3, a scalar
// and an array, each its registers, rows, columns and elements, and a cube sampler.
float4x4 M : register(c0);
row_major float4x4 R : register(c4);
float3 L : register(c8);
float S : register(c9);
float4 Arr[3] : register(c10);
samplerCUBE Env : register(s2);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    return mul(uv, M) + mul(R, uv) + float4(L, S) + Arr[1] + texCUBE(Env, uv.xyz);
}
