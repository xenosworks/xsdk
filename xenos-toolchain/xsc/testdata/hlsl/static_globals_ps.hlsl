// Static globals, which glslang makes Private variables: two set in a helper and one from an input, which is set again,
// each read after.
float4 Scale : register(c0);
static float4 tint;
static float2 doubled;
static float4 kept;

void prepare(float2 uv)
{
    doubled = uv * 2.0;
    tint = Scale * uv.x;
}

float4 ps_main(float2 uv : TEXCOORD0, float4 colour : TEXCOORD1) : COLOR0
{
    kept = colour;
    prepare(uv);
    kept = kept * 0.5 - tint;
    return tint + doubled.xyxy - kept;
}
