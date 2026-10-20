uniform float4 u_tint_tint : register(c0);

uniform sampler2D u_texture;

static float4 o_color;
static float2 v_uv;

struct SPIRV_Cross_Input
{
    float2 v_uv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    o_color = tex2D(u_texture, v_uv) * u_tint_tint;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    v_uv = stage_input.v_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
