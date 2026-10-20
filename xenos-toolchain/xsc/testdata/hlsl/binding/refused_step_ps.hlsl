// A loop an int counts from 1: a title setting Count by name couldn't give its loop constant the passes, so refused.
float4 Step : register(c0);
int Count : register(i0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 1; i < Count; i++)
        c += Step;
    return c;
}
