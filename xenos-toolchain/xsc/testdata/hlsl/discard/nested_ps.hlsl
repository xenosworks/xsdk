// nested.frag's twin: a discard after a return in an if inside another.
float4 ps_main(float2 uv : TEXCOORD0) : COLOR0
{
    if (uv.y > 0.25) {
        if (uv.x > 0.5)
            return float4(uv, 0, 1);
    }
    discard;
    return float4(uv, 1, 1);
}
