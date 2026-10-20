uniform float4 upos_mtx[30] : register(c0);

uniform float4 uview_viewport_sizes : register(c30);
uniform float4 uview_line_params : register(c31);
uniform float4 uview_proj[4] : register(c32);
uniform float4 uview_tex_mtx[30] : register(c36);
uniform float4 uview_channel_colors[8] : register(c66);
uniform float4 uview_postmtx[60] : register(c74);
uniform float4 uview_fog[5] : register(c134);
uniform float4 uview_ind_mtx[6] : register(c139);

uniform float4 upush_pos_mtx[3] : register(c145);


static float4 gl_Position;
static float in_pnmtxidx;
static float3 in_pos;

struct SPIRV_Cross_Input
{
    float3 in_pos : TEXCOORD0;
    float in_pnmtxidx : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 gl_Position : POSITION;
};

void vert_main()
{
    int pnmtx = int(in_pnmtxidx);
    float4 pos4 = float4(in_pos, 1.0f);
    float4 mv_pos4 = float4(float3(dot(upos_mtx[pnmtx], pos4), dot(upos_mtx[pnmtx + 1], pos4), dot(upos_mtx[pnmtx + 2], pos4)), 1.0f);
    float4 clip_pos = float4(dot(uview_proj[0], mv_pos4), dot(uview_proj[1], mv_pos4), dot(uview_proj[2], mv_pos4), dot(uview_proj[3], mv_pos4));
    gl_Position = clip_pos * float4(1.0f, -1.0f, 1.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_pnmtxidx = stage_input.in_pnmtxidx;
    in_pos = stage_input.in_pos;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    return stage_output;
}
