// A clip on a float4 alone, the kill-if's.
float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    clip(float4(uv.x - 0.25, uv.y - 0.25, 0.75 - uv.x, 0.75 - uv.y));
    return colour;
}
