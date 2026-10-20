// A hardware loop an int in $Global counts, left on data, and its counter read after it.
float4 Step : register(c0);
float4 Limit : register(c1);
int Count : register(c2);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 s = uv;
    int i;
    for (i = 0; i < Count; i++) {
        s += Step;
        if (s.x > Limit.x)
            break;
    }
    return float4(s.xyz, i);
}
