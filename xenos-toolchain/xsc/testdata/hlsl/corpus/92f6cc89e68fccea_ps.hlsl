static float4 o_color;

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float3 s0_color = 0.0f.xxx;
    float s0_alpha = 0.0f;
    o_color = float4(s0_color, s0_alpha);
}

SPIRV_Cross_Output main()
{
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
