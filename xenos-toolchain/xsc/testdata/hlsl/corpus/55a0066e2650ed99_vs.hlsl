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
static float2 in_tex0_uv;
static float2 out_tex0_uv;

struct SPIRV_Cross_Input
{
    float3 in_pos : TEXCOORD0;
    float4 in_clr0 : TEXCOORD1;
    float2 in_tex0_uv : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 out_cc0 : TEXCOORD0;
    float2 out_tex0_uv : TEXCOORD1;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    float4 pos4 = float4(in_pos, 1.0f);
    float4 mv_pos4 = float4(float3(dot(upush_pos_mtx[0], pos4), dot(upush_pos_mtx[1], pos4), dot(upush_pos_mtx[2], pos4)), 1.0f);
    float4 clip_pos = float4(dot(uview_proj[0], mv_pos4), dot(uview_proj[1], mv_pos4), dot(uview_proj[2], mv_pos4), dot(uview_proj[3], mv_pos4));
    gl_Position = clip_pos * float4(1.0f, -1.0f, 1.0f, 1.0f);
    float4 cc0 = float4(in_clr0.xyz, in_clr0.w);
    out_cc0 = cc0;
    float4 tc0 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc0_mtx = tc0.xyz;
    float3 tc0_tmp = float3(tc0_mtx.xy, 1.0f);
    float4 tc0_post = float4(tc0_tmp, 1.0f);
    float3 tc0_proj = float3(dot(uview_postmtx[0], tc0_post), dot(uview_postmtx[1], tc0_post), dot(uview_postmtx[2], tc0_post));
    out_tex0_uv = tc0_proj.xy;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_pos = stage_input.in_pos;
    in_clr0 = stage_input.in_clr0;
    in_tex0_uv = stage_input.in_tex0_uv;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.out_cc0 = out_cc0;
    stage_output.out_tex0_uv = out_tex0_uv;
    return stage_output;
}
