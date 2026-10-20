uniform float4 uview_viewport_sizes : register(c0);
uniform float4 uview_line_params : register(c1);
uniform float4 uview_proj[4] : register(c2);
uniform float4 uview_tex_mtx[30] : register(c6);
uniform float4 uview_channel_colors[8] : register(c36);
uniform float4 uview_postmtx[60] : register(c44);
uniform float4 uview_fog[5] : register(c104);
uniform float4 uview_ind_mtx[6] : register(c109);
uniform float4 uview_depth_range : register(c115);

uniform sampler2D fog_range_table;

static float4 gl_FragCoord;
static float4 in_cc0;
static float2 in_fog_zw;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float4 in_cc0 : TEXCOORD0;
    float2 in_fog_zw : TEXCOORD1;
    float4 gl_FragCoord : VPOS;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float3 s0_color = 0.0f.xxx;
    float s0_alpha = clamp(0.0f + in_cc0.w, 0.0f, 1.0f);
    float fog_ndc = in_fog_zw.x / in_fog_zw.y;
    float fog_depth = uview_depth_range.y - (fog_ndc * (uview_depth_range.y - uview_depth_range.x));
    float fog_base = uview_fog[1].x / (uview_fog[1].y - fog_depth);
    float fog_column = gl_FragCoord.x * 0.015625f;
    float4 fog_range = tex2D(fog_range_table, float2(fog_column, (floor(fog_column) * 0.03125f) + 0.015625f));
    float fog_f = clamp((fog_base * fog_range.x) - uview_fog[1].z, 0.0f, 1.0f);
    float fog_z = fog_f;
    float3 fog_color = lerp(s0_color, uview_fog[0].xyz, clamp(fog_z, 0.0f, 1.0f).xxx);
    o_color = float4(fog_color, s0_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord + float4(0.5f, 0.5f, 0.0f, 0.0f);
    in_cc0 = stage_input.in_cc0;
    in_fog_zw = stage_input.in_fog_zw;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
