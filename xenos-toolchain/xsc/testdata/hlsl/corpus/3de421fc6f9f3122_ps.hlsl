static float4 in_cc0;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float4 in_cc0 : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float3 s0_color = clamp(0.0f.xxx + in_cc0.xyz, 0.0f.xxx, 1.0f.xxx);
    float s0_alpha = clamp(0.0f + in_cc0.w, 0.0f, 1.0f);
    float alpha_byte = floor((s0_alpha * 255.0f) + 0.5f);
    float alpha_pass = step(0.5f, alpha_byte - 0.0f);
    if (alpha_pass < 0.5f)
    {
        clip(-1.0f);
    }
    o_color = float4(s0_color, s0_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_cc0 = stage_input.in_cc0;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
