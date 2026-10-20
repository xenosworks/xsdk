uniform float4 uview_viewport_sizes : register(c0);
uniform float4 uview_line_params : register(c1);
uniform float4 uview_proj[4] : register(c2);
uniform float4 uview_tex_mtx[30] : register(c6);
uniform float4 uview_channel_colors[8] : register(c36);
uniform float4 uview_postmtx[60] : register(c44);
uniform float4 uview_fog[5] : register(c104);
uniform float4 uview_ind_mtx[6] : register(c109);

uniform float4 ulights_lights[40] : register(c115);
uniform float4 ulights_light_state : register(c155);

uniform float4 upush_pos_mtx[3] : register(c156);
uniform float4 upush_nrm_mtx[3] : register(c159);


static float4 gl_Position;
static float3 in_pos;
static float3 in_nrm;
static float4 out_cc0;
static float2 in_tex0_uv;
static float2 out_tex0_uv;
static float2 in_tex1_uv;
static float2 out_tex1_uv;
static float2 out_fog_zw;

struct SPIRV_Cross_Input
{
    float3 in_pos : TEXCOORD0;
    float2 in_tex0_uv : TEXCOORD2;
    float2 in_tex1_uv : TEXCOORD3;
    float3 in_nrm : TEXCOORD4;
};

struct SPIRV_Cross_Output
{
    float4 out_cc0 : TEXCOORD0;
    float2 out_tex0_uv : TEXCOORD1;
    float2 out_tex1_uv : TEXCOORD2;
    float2 out_fog_zw : TEXCOORD3;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    float4 pos4 = float4(in_pos, 1.0f);
    float4 mv_pos4 = float4(float3(dot(upush_pos_mtx[0], pos4), dot(upush_pos_mtx[1], pos4), dot(upush_pos_mtx[2], pos4)), 1.0f);
    float4 clip_pos = float4(dot(uview_proj[0], mv_pos4), dot(uview_proj[1], mv_pos4), dot(uview_proj[2], mv_pos4), dot(uview_proj[3], mv_pos4));
    gl_Position = clip_pos * float4(1.0f, -1.0f, 1.0f, 1.0f);
    float4 nrm4 = float4(in_nrm, 0.0f);
    float3 nrm_tmp = float3(dot(upush_nrm_mtx[0], nrm4), dot(upush_nrm_mtx[1], nrm4), dot(upush_nrm_mtx[2], nrm4));
    float nrm_len2 = dot(nrm_tmp, nrm_tmp);
    float3 mv_nrm = nrm_tmp * lerp(1.0f, rsqrt(max(nrm_len2, 1.0000000133514319600180897396058e-10f)), step(1.0000000133514319600180897396058e-10f, nrm_len2));
    float3 light0_vec = ulights_lights[0].xyz - mv_pos4.xyz;
    float light0_dist2 = dot(light0_vec, light0_vec);
    float light0_dist = sqrt(light0_dist2);
    float3 light0_dir = light0_vec / light0_dist.xxx;
    float cc0_cos0 = clamp(dot(light0_dir, ulights_lights[1].xyz), 0.0f, 1.0f);
    float cc0_attn0 = max(0.0f, dot(ulights_lights[3].xyz, float3(1.0f, cc0_cos0, cc0_cos0 * cc0_cos0)) / dot(ulights_lights[4].xyz, float3(1.0f, light0_dist, light0_dist2)));
    float cc0_diff0 = max(0.0f, dot(light0_dir, mv_nrm));
    float4 cc0_sum0 = uview_channel_colors[0] + (ulights_lights[2] * (cc0_attn0 * cc0_diff0));
    float4 cc0_lit = uview_channel_colors[1] * clamp(cc0_sum0, 0.0f.xxxx, 1.0f.xxxx);
    float4 cc0 = float4(cc0_lit.xyz, uview_channel_colors[3].w);
    out_cc0 = cc0;
    float4 tc0 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc0_mtx = float3(dot(uview_tex_mtx[0], tc0), dot(uview_tex_mtx[1], tc0), dot(uview_tex_mtx[2], tc0));
    float3 tc0_tmp = float3(tc0_mtx.xy, 1.0f);
    out_tex0_uv = tc0_tmp.xy;
    float4 tc1 = float4(in_tex1_uv, 1.0f, 1.0f);
    float3 tc1_mtx = float3(dot(uview_tex_mtx[3], tc1), dot(uview_tex_mtx[4], tc1), dot(uview_tex_mtx[5], tc1));
    float3 tc1_tmp = float3(tc1_mtx.xy, 1.0f);
    out_tex1_uv = tc1_tmp.xy;
    out_fog_zw = clip_pos.zw;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_pos = stage_input.in_pos;
    in_nrm = stage_input.in_nrm;
    in_tex0_uv = stage_input.in_tex0_uv;
    in_tex1_uv = stage_input.in_tex1_uv;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.out_cc0 = out_cc0;
    stage_output.out_tex0_uv = out_tex0_uv;
    stage_output.out_tex1_uv = out_tex1_uv;
    stage_output.out_fog_zw = out_fog_zw;
    return stage_output;
}
