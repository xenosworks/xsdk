uniform sampler2D source;

static float2 in_uv;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float2 in_uv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float4 z24 = min(floor((clamp(1.0f.xxxx - tex2D(source, in_uv).xxxx, 0.0f.xxxx, 1.0f.xxxx) * 16777215.0f) + 0.5f.xxxx), 16777215.0f.xxxx);
    float4 high = floor(z24 * 1.52587890625e-05f);
    o_color = high * 0.0039215688593685626983642578125f;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_uv = stage_input.in_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
