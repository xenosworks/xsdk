// mul(v, M) with M row_major: D3D9 puts row i of M in c(4 + i).
float4 Pad : register(c0);
row_major float4x4 M : register(c4);
float4 vs_main(float4 pos : POSITION) : SV_Position { return mul(pos, M) + Pad; }
