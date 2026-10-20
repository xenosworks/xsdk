// Refused: a float3x3 output cast from a float4x4 in $Global, which glslang lays by rows, where an interpolator takes a
// column.
float4x4 World : register(c0);

struct VS_OUT {
    float4 pos : POSITION;
    float3x3 frame : TEXCOORD0;
};

VS_OUT vs_main(float4 pos : POSITION)
{
    VS_OUT o;
    o.pos = pos;
    o.frame = (float3x3)World;
    return o;
}
