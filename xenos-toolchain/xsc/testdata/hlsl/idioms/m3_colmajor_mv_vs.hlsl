// mul(M, v) with M default (column_major): column j of M in c(4 + j), the result M times the column vector v.
float4 Pad : register(c0);
float4x4 M : register(c4);
float4 vs_main(float4 pos : POSITION) : SV_Position { return mul(M, pos) + Pad; }
