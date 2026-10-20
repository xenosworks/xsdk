// A scalar int input and an int4's components made floats, one negated, each in a lane of its own.
float4 vs_main(float4 pos : POSITION, int k : BLENDINDICES0, int4 idx : BLENDINDICES1) : POSITION
{
    return float4(pos.x, -(float)k, (float)idx.z, (float)(idx.y + k));
}
