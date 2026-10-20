uniform sampler2D u_texture;

static float4 o_color;
static float2 v_uv;
static float4 v_color;

struct SPIRV_Cross_Input
{
    float2 v_uv : TEXCOORD0;
    float4 v_color : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    o_color = tex2D(u_texture, v_uv) * v_color;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    v_uv = stage_input.v_uv;
    v_color = stage_input.v_color;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
