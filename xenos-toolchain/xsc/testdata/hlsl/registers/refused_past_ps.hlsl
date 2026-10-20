// A bool an if reads at b130, past the 128 boolean constants a pixel shader takes: refused. The loop has the unroller
// run, so the if's arms go in execs on the bool's boolean constant.
float4 Step : register(c0);
bool Far : register(b130);

float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float4 c = uv;
    for (int i = 0; i < 2; i++)
        c += Step;
    if (Far)
        c = c.wzyx * 2.0;
    return c;
}
