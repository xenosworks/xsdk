// A point's size alone as an out parameter beside the position, which glslang makes a PointSize of its own.
float4 Size : register(c0);

void vs_main(float4 pos : POSITION, out float4 opos : POSITION, out float psize : PSIZE)
{
    opos = pos;
    psize = Size.x * pos.w;
}
