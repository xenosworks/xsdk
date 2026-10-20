// Refused: a local array of TEXCOORD0's components at a run-time index, which only a constant array's literal
// registers give a0 an array to read.
float4 ps_main(float4 uv : TEXCOORD0) : COLOR0
{
    float a[3] = { uv.x, uv.y, uv.z };
    return a[(int)(uv.w * 2.0)];
}
