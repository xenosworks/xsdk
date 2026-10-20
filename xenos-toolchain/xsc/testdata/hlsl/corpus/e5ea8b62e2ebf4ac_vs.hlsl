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
static float2 out_tex1_uv;
static float2 out_tex2_uv;
static float2 out_tex3_uv;
static float2 out_fog_zw;

struct SPIRV_Cross_Input
{
    float3 in_pos : TEXCOORD0;
    float2 in_tex0_uv : TEXCOORD2;
    float3 in_nrm : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float4 out_cc0 : TEXCOORD0;
    float2 out_tex0_uv : TEXCOORD1;
    float2 out_tex1_uv : TEXCOORD2;
    float2 out_tex2_uv : TEXCOORD3;
    float2 out_tex3_uv : TEXCOORD4;
    float2 out_fog_zw : TEXCOORD5;
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
    float3 light1_vec = ulights_lights[5].xyz - mv_pos4.xyz;
    float light1_dist2 = dot(light1_vec, light1_vec);
    float light1_dist = sqrt(light1_dist2);
    float3 light1_dir = light1_vec / light1_dist.xxx;
    float cc0_cos1 = clamp(dot(light1_dir, ulights_lights[6].xyz), 0.0f, 1.0f);
    float cc0_attn1 = max(0.0f, dot(ulights_lights[8].xyz, float3(1.0f, cc0_cos1, cc0_cos1 * cc0_cos1)) / dot(ulights_lights[9].xyz, float3(1.0f, light1_dist, light1_dist2)));
    float cc0_diff1 = max(0.0f, dot(light1_dir, mv_nrm));
    float4 cc0_sum1 = cc0_sum0 + (ulights_lights[7] * (cc0_attn1 * cc0_diff1));
    float3 light2_vec = ulights_lights[10].xyz - mv_pos4.xyz;
    float light2_dist2 = dot(light2_vec, light2_vec);
    float light2_dist = sqrt(light2_dist2);
    float3 light2_dir = light2_vec / light2_dist.xxx;
    float cc0_cos2 = clamp(dot(light2_dir, ulights_lights[11].xyz), 0.0f, 1.0f);
    float cc0_attn2 = max(0.0f, dot(ulights_lights[13].xyz, float3(1.0f, cc0_cos2, cc0_cos2 * cc0_cos2)) / dot(ulights_lights[14].xyz, float3(1.0f, light2_dist, light2_dist2)));
    float cc0_diff2 = max(0.0f, dot(light2_dir, mv_nrm));
    float4 cc0_sum2 = cc0_sum1 + (ulights_lights[12] * (cc0_attn2 * cc0_diff2));
    float3 light3_vec = ulights_lights[15].xyz - mv_pos4.xyz;
    float light3_dist2 = dot(light3_vec, light3_vec);
    float light3_dist = sqrt(light3_dist2);
    float3 light3_dir = light3_vec / light3_dist.xxx;
    float cc0_cos3 = clamp(dot(light3_dir, ulights_lights[16].xyz), 0.0f, 1.0f);
    float cc0_attn3 = max(0.0f, dot(ulights_lights[18].xyz, float3(1.0f, cc0_cos3, cc0_cos3 * cc0_cos3)) / dot(ulights_lights[19].xyz, float3(1.0f, light3_dist, light3_dist2)));
    float cc0_diff3 = max(0.0f, dot(light3_dir, mv_nrm));
    float4 cc0_sum3 = cc0_sum2 + (ulights_lights[17] * (cc0_attn3 * cc0_diff3));
    float3 light4_vec = ulights_lights[20].xyz - mv_pos4.xyz;
    float light4_dist2 = dot(light4_vec, light4_vec);
    float light4_dist = sqrt(light4_dist2);
    float3 light4_dir = light4_vec / light4_dist.xxx;
    float cc0_cos4 = clamp(dot(light4_dir, ulights_lights[21].xyz), 0.0f, 1.0f);
    float cc0_attn4 = max(0.0f, dot(ulights_lights[23].xyz, float3(1.0f, cc0_cos4, cc0_cos4 * cc0_cos4)) / dot(ulights_lights[24].xyz, float3(1.0f, light4_dist, light4_dist2)));
    float cc0_diff4 = max(0.0f, dot(light4_dir, mv_nrm));
    float4 cc0_sum4 = cc0_sum3 + (ulights_lights[22] * (cc0_attn4 * cc0_diff4));
    float3 light5_vec = ulights_lights[25].xyz - mv_pos4.xyz;
    float light5_dist2 = dot(light5_vec, light5_vec);
    float light5_dist = sqrt(light5_dist2);
    float3 light5_dir = light5_vec / light5_dist.xxx;
    float cc0_cos5 = clamp(dot(light5_dir, ulights_lights[26].xyz), 0.0f, 1.0f);
    float cc0_attn5 = max(0.0f, dot(ulights_lights[28].xyz, float3(1.0f, cc0_cos5, cc0_cos5 * cc0_cos5)) / dot(ulights_lights[29].xyz, float3(1.0f, light5_dist, light5_dist2)));
    float cc0_diff5 = max(0.0f, dot(light5_dir, mv_nrm));
    float4 cc0_sum5 = cc0_sum4 + (ulights_lights[27] * (cc0_attn5 * cc0_diff5));
    float3 light6_vec = ulights_lights[30].xyz - mv_pos4.xyz;
    float light6_dist2 = dot(light6_vec, light6_vec);
    float light6_dist = sqrt(light6_dist2);
    float3 light6_dir = light6_vec / light6_dist.xxx;
    float cc0_cos6 = clamp(dot(light6_dir, ulights_lights[31].xyz), 0.0f, 1.0f);
    float cc0_attn6 = max(0.0f, dot(ulights_lights[33].xyz, float3(1.0f, cc0_cos6, cc0_cos6 * cc0_cos6)) / dot(ulights_lights[34].xyz, float3(1.0f, light6_dist, light6_dist2)));
    float cc0_diff6 = max(0.0f, dot(light6_dir, mv_nrm));
    float4 cc0_sum6 = cc0_sum5 + (ulights_lights[32] * (cc0_attn6 * cc0_diff6));
    float3 light7_vec = ulights_lights[35].xyz - mv_pos4.xyz;
    float light7_dist2 = dot(light7_vec, light7_vec);
    float light7_dist = sqrt(light7_dist2);
    float3 light7_dir = light7_vec / light7_dist.xxx;
    float cc0_cos7 = clamp(dot(light7_dir, ulights_lights[36].xyz), 0.0f, 1.0f);
    float cc0_attn7 = max(0.0f, dot(ulights_lights[38].xyz, float3(1.0f, cc0_cos7, cc0_cos7 * cc0_cos7)) / dot(ulights_lights[39].xyz, float3(1.0f, light7_dist, light7_dist2)));
    float cc0_diff7 = max(0.0f, dot(light7_dir, mv_nrm));
    float4 cc0_sum7 = cc0_sum6 + (ulights_lights[37] * (cc0_attn7 * cc0_diff7));
    float4 cc0_lit = uview_channel_colors[1] * clamp(cc0_sum7, 0.0f.xxxx, 1.0f.xxxx);
    float4 cc0 = float4(cc0_lit.xyz, uview_channel_colors[3].w);
    out_cc0 = cc0;
    float4 tc0 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc0_mtx = tc0.xyz;
    float3 tc0_tmp = float3(tc0_mtx.xy, 1.0f);
    out_tex0_uv = tc0_tmp.xy;
    float4 tc1 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc1_mtx = tc1.xyz;
    float3 tc1_tmp = float3(tc1_mtx.xy, 1.0f);
    out_tex1_uv = tc1_tmp.xy;
    float4 tc2 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc2_mtx = float3(dot(uview_tex_mtx[6], tc2), dot(uview_tex_mtx[7], tc2), dot(uview_tex_mtx[8], tc2));
    float3 tc2_tmp = float3(tc2_mtx.xy, 1.0f);
    out_tex2_uv = tc2_tmp.xy;
    float4 tc3 = float4(in_tex0_uv, 1.0f, 1.0f);
    float3 tc3_mtx = tc3.xyz;
    float3 tc3_tmp = float3(tc3_mtx.xy, 1.0f);
    out_tex3_uv = tc3_tmp.xy;
    out_fog_zw = clip_pos.zw;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_pos = stage_input.in_pos;
    in_nrm = stage_input.in_nrm;
    in_tex0_uv = stage_input.in_tex0_uv;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.out_cc0 = out_cc0;
    stage_output.out_tex0_uv = out_tex0_uv;
    stage_output.out_tex1_uv = out_tex1_uv;
    stage_output.out_tex2_uv = out_tex2_uv;
    stage_output.out_tex3_uv = out_tex3_uv;
    stage_output.out_fog_zw = out_fog_zw;
    return stage_output;
}
