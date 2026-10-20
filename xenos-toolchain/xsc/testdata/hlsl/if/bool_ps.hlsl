// An if on a bool at register(b2), changing a value.
bool Flip : register(b2);
float4 Tint : register(c1);

float4 ps_main(float2 uv : TEXCOORD0, float4 colour : COLOR0) : COLOR0
{
    float4 c = colour;
    if (Flip)
        c.rgb = c.gbr * Tint.rgb;
    return c;
}
