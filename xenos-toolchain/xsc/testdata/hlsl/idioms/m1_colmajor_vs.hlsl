// mul(v, M) with M default (column_major): D3D9 puts column j of M in c(4 + j).
float4 Pad : register(c0);
float4x4 M : register(c4);
float4 vs_main(float4 pos : POSITION) : SV_Position { return mul(pos, M) + Pad; }
