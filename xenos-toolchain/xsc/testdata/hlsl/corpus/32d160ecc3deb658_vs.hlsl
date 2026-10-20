uniform float4 uview_viewport_sizes : register(c0);
uniform float4 uview_line_params : register(c1);
uniform float4 uview_proj[4] : register(c2);
uniform float4 uview_tex_mtx[30] : register(c6);
uniform float4 uview_channel_colors[8] : register(c36);
uniform float4 uview_postmtx[60] : register(c44);
uniform float4 uview_fog[5] : register(c104);
uniform float4 uview_ind_mtx[6] : register(c109);

uniform float4 upush_pos_mtx[3] : register(c115);


static float4 gl_Position;
static float3 in_pos;
static float4 in_clr0;
static float4 out_cc0;

struct SPIRV_Cross_Input
{
    float3 in_pos : TEXCOORD0;
    float4 in_clr0 : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 out_cc0 : TEXCOORD0;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    float4 pos4 = float4(in_pos, 1.0f);
    float4 mv_pos4 = float4(float3(dot(upush_pos_mtx[0], pos4), dot(upush_pos_mtx[1], pos4), dot(upush_pos_mtx[2], pos4)), 1.0f);
    float4 clip_pos = float4(dot(uview_proj[0], mv_pos4), dot(uview_proj[1], mv_pos4), dot(uview_proj[2], mv_pos4), dot(uview_proj[3], mv_pos4));
    gl_Position = clip_pos * float4(1.0f, -1.0f, 1.0f, 1.0f);
    float4 cc0 = float4(uview_channel_colors[1].xyz, in_clr0.w);
    out_cc0 = cc0;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_pos = stage_input.in_pos;
    in_clr0 = stage_input.in_clr0;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.out_cc0 = out_cc0;
    return stage_output;
}
