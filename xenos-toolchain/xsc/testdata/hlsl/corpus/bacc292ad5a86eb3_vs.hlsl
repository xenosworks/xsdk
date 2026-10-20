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
static float2 in_tex0_uv;
static float2 out_tex0_uv;
static float2 out_tex1_uv;
static float2 out_tex2_uv;
static float2 out_tex3_uv;
static float2 out_tex4_uv;
static float2 out_tex5_uv;
static float2 out_tex6_uv;
static float2 out_tex7_uv;

struct SPIRV_Cross_Input
{
    float3 in_pos : TEXCOORD0;
    float2 in_tex0_uv : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float2 out_tex0_uv : TEXCOORD0;
    float2 out_tex1_uv : TEXCOORD1;
    float2 out_tex2_uv : TEXCOORD2;
    float2 out_tex3_uv : TEXCOORD3;
    float2 out_tex4_uv : TEXCOORD4;
    float2 out_tex5_uv : TEXCOORD5;
    float2 out_tex6_uv : TEXCOORD6;
    float2 out_tex7_uv : TEXCOORD7;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    float4 pos4 = float4(in_pos, 1.0f);
    float4 mv_pos4 = float4(float3(dot(upush_pos_mtx[0], pos4), dot(upush_pos_mtx[1], pos4), dot(upush_pos_mtx[2], pos4)), 1.0f);
    float4 clip_pos = float4(dot(uview_proj[0], mv_pos4), dot(uview_proj[1], mv_pos4), dot(uview_proj[2], mv_pos4), dot(uview_proj[3], mv_pos4));
    gl_Position = clip_pos * float4(1.0f, -1.0f, 1.0f, 1.0f);
    float4 tc0 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc0_mtx = float3(dot(uview_tex_mtx[0], tc0), dot(uview_tex_mtx[1], tc0), dot(uview_tex_mtx[2], tc0));
    float3 tc0_tmp = float3(tc0_mtx.xy, 1.0f);
    out_tex0_uv = tc0_tmp.xy;
    float4 tc1 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc1_mtx = float3(dot(uview_tex_mtx[3], tc1), dot(uview_tex_mtx[4], tc1), dot(uview_tex_mtx[5], tc1));
    float3 tc1_tmp = float3(tc1_mtx.xy, 1.0f);
    out_tex1_uv = tc1_tmp.xy;
    float4 tc2 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc2_mtx = float3(dot(uview_tex_mtx[6], tc2), dot(uview_tex_mtx[7], tc2), dot(uview_tex_mtx[8], tc2));
    float3 tc2_tmp = float3(tc2_mtx.xy, 1.0f);
    out_tex2_uv = tc2_tmp.xy;
    float4 tc3 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc3_mtx = float3(dot(uview_tex_mtx[9], tc3), dot(uview_tex_mtx[10], tc3), dot(uview_tex_mtx[11], tc3));
    float3 tc3_tmp = float3(tc3_mtx.xy, 1.0f);
    out_tex3_uv = tc3_tmp.xy;
    float4 tc4 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc4_mtx = float3(dot(uview_tex_mtx[12], tc4), dot(uview_tex_mtx[13], tc4), dot(uview_tex_mtx[14], tc4));
    float3 tc4_tmp = float3(tc4_mtx.xy, 1.0f);
    out_tex4_uv = tc4_tmp.xy;
    float4 tc5 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc5_mtx = float3(dot(uview_tex_mtx[15], tc5), dot(uview_tex_mtx[16], tc5), dot(uview_tex_mtx[17], tc5));
    float3 tc5_tmp = float3(tc5_mtx.xy, 1.0f);
    out_tex5_uv = tc5_tmp.xy;
    float4 tc6 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc6_mtx = float3(dot(uview_tex_mtx[18], tc6), dot(uview_tex_mtx[19], tc6), dot(uview_tex_mtx[20], tc6));
    float3 tc6_tmp = float3(tc6_mtx.xy, 1.0f);
    out_tex6_uv = tc6_tmp.xy;
    float4 tc7 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc7_mtx = float3(dot(uview_tex_mtx[21], tc7), dot(uview_tex_mtx[22], tc7), dot(uview_tex_mtx[23], tc7));
    float3 tc7_tmp = float3(tc7_mtx.xy, 1.0f);
    out_tex7_uv = tc7_tmp.xy;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_pos = stage_input.in_pos;
    in_tex0_uv = stage_input.in_tex0_uv;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.out_tex0_uv = out_tex0_uv;
    stage_output.out_tex1_uv = out_tex1_uv;
    stage_output.out_tex2_uv = out_tex2_uv;
    stage_output.out_tex3_uv = out_tex3_uv;
    stage_output.out_tex4_uv = out_tex4_uv;
    stage_output.out_tex5_uv = out_tex5_uv;
    stage_output.out_tex6_uv = out_tex6_uv;
    stage_output.out_tex7_uv = out_tex7_uv;
    return stage_output;
}
