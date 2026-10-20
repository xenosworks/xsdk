uniform float4 pc_tint : register(c4);

uniform sampler2D u_texture;

static float2 v_uv;
static float4 o_color;

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
    float4 c = tex2D(u_texture, v_uv);
    float l = dot(c.xyz, float3(0.2989999949932098388671875f, 0.58700001239776611328125f, 0.114000000059604644775390625f));
    o_color = float4(l, l, l, c.w) * pc_tint;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    v_uv = stage_input.v_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
