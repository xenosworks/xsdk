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
    float4 sampled = tex2D(source, in_uv);
    float4 c = float4(sampled.xyz, 1.0f);
    o_color = (floor(c.x * 16.0f) / 15.0f).xxxx;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_uv = stage_input.in_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
