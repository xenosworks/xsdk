// Samplers at s1 and s3 only, one sampler without a register, and a constant at c5: where xost puts each.
sampler2D A : register(s3);
sampler2D B : register(s1);
sampler2D C;
float4 K : register(c5);
float4 ps_main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(A, uv) * tex2D(B, uv) + tex2D(C, uv) * K; }
