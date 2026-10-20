// A hardware loop an int in $Global counts, left on data, whose exits aren't its header's values alone: a value set
// before it, the sum, which a value read after the loop takes from the header too, and that value.
float4 Threshold : register(c0);
int Count : register(c1);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float s = 0;
    float last = 0;
    float found = -1;
    for (int i = 0; i < Count; i++) {
        last = s;
        s += uv.x + i * 0.25;
        if (s > Threshold.x) {
            found = i;
            break;
        }
    }
    return float4(s, last, found, 1);
}
