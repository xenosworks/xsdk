// A vertex shader's i#: Count at i2 is loop constant 2, the vertex half's; Flag at b1, an if's, is a value, as a
// vertex shader's bools are.
float4 Step : register(c0);
int Count : register(i2);
bool Flag : register(b1);

float4 vs_main(float4 pos : POSITION) : POSITION
{
    float4 p = pos;
    for (int i = 0; i < Count; i++)
        p += Step;
    if (Flag)
        p = p.yxwz * 2.0;
    return p;
}
