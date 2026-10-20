// HLSL's fmod truncates, so each result has the sign of x, where GLSL's mod floors: a vector's lanes from TEXCOORD0
// and TEXCOORD1, and scalars by a constant y, folded constants and a register's y.
float4 Y : register(c0);

void ps_main(float4 x : TEXCOORD0, float4 y : TEXCOORD1, out float4 lanes : COLOR0, out float4 scalars : COLOR1)
{
    lanes = fmod(x, y);
    scalars = float4(fmod(x.y, -2.0), fmod(-7.5, 2.0), fmod(7.5, -2.0), fmod(x.w, Y.w));
}
