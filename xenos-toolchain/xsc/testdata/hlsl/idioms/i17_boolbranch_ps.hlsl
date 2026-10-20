// A bool constant choosing between two paths, the bool in a float register of its own.
sampler2D Tex : register(s0);
bool UseTexture : register(c0);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    return UseTexture ? tex2D(Tex, uv) : float4(1, 1, 1, 1);
}
