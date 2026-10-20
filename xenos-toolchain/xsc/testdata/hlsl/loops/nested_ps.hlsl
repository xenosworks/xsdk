// Nested unrolled loops, the inner one left on data, the outer reading what it leaves.
float4 Scale : register(c0);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float s = 0;
    float t = 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            s += uv.x * i + uv.y * j * Scale.x;
            if (s > Scale.y)
                break;
        }
        t += s;
    }
    return float4(s, t, uv.z, 1);
}
