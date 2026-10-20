// A vertex input whose semantic is none of D3D9's declaration usages.
float4 vs_main(float4 pos : POSITION, float4 extra : MYDATA) : POSITION
{
    return pos + extra;
}
