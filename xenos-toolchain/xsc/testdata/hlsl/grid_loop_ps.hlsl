// The grid's loop: a hardware loop an int in $Global counts, each pixel leaving it once its red passes TEXCOORD0's x,
// red and green what it carries, blue the pass it left at.
float4 Step : register(c0);
int Count : register(c1);

float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = float4(0.0, 0.0, 0.0, 1.0);
    int i;
    for (i = 0; i < Count; i++) {
        c.rg += Step.xy;
        if (c.r > uv.x)
            break;
    }
    c.b = i * Step.z;
    return c;
}
