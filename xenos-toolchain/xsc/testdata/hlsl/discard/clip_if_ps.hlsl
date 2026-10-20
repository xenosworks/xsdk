// A clip, then an if changing a value: glslang's if around the clip's discard beside the other if.
float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = float4(uv, 0.25, 1);
    clip(uv.y - 0.25);
    if (uv.x > 0.5)
        c.rgb *= 0.5;
    return c;
}
