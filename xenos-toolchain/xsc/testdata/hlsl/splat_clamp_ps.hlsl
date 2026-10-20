// clamp to scalar bounds, which glslang folds into constant splats.
float4 ps_main(float4 v : TEXCOORD0, float2 w : TEXCOORD1) : COLOR0
{
    return clamp(v, -0.5, 0.75) + clamp(w, 0.25, 2.0).xyxy;
}
