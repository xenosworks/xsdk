uniform float4 umat_tevreg[4] : register(c0);
uniform float4 umat_kcolor[4] : register(c4);
uniform float4 umat_texcoord_scale[8] : register(c8);
uniform float4 umat_tex_size_bias[8] : register(c16);

uniform float4 uview_viewport_sizes : register(c24);
uniform float4 uview_line_params : register(c25);
uniform float4 uview_proj[4] : register(c26);
uniform float4 uview_tex_mtx[30] : register(c30);
uniform float4 uview_channel_colors[8] : register(c60);
uniform float4 uview_postmtx[60] : register(c68);
uniform float4 uview_fog[5] : register(c128);
uniform float4 uview_ind_mtx[6] : register(c133);
uniform float4 uview_depth_range : register(c139);

uniform sampler2D tex1;
uniform sampler2D tex0;
uniform sampler2D tex2;

static float2 in_tex0_uv;
static float2 in_tex1_uv;
static float2 in_tex2_uv;
static float4 in_cc0;
static float2 in_fog_zw;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float4 in_cc0 : TEXCOORD0;
    float2 in_tex0_uv : TEXCOORD1;
    float2 in_tex1_uv : TEXCOORD2;
    float2 in_tex2_uv : TEXCOORD3;
    float2 in_fog_zw : TEXCOORD4;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float2 tex0_uv = in_tex0_uv;
    float2 tex1_uv = in_tex1_uv;
    float2 tex2_uv = in_tex2_uv;
    float4 sampled0 = tex2D(tex1, tex1_uv);
    float4 sampled1 = tex2D(tex0, tex0_uv);
    float4 sampled2 = tex2D(tex2, tex2_uv);
    float3 s0_color = clamp(lerp(sampled0.xyz, 0.0f.xxx, 0.0f.xxx) + 0.0f.xxx, 0.0f.xxx, 1.0f.xxx);
    float s0_alpha = clamp(lerp(0.0f, umat_tevreg[2].w, in_cc0.w) + 0.0f, 0.0f, 1.0f);
    float3 s1_color = clamp(lerp(0.0f.xxx, umat_kcolor[1].w.xxx, sampled1.xyz) + s0_color, 0.0f.xxx, 1.0f.xxx);
    float s1_alpha = clamp(lerp(0.0f, 0.125f, in_cc0.w) + s0_alpha, 0.0f, 1.0f);
    float3 s2_color = clamp(lerp(0.0f.xxx, sampled2.w.xxx, sampled2.xyz) + s1_color, 0.0f.xxx, 1.0f.xxx);
    float s2_alpha = clamp(lerp(0.0f, umat_tevreg[3].w, sampled2.w) + s1_alpha, 0.0f, 1.0f);
    float fog_ndc = in_fog_zw.x / in_fog_zw.y;
    float fog_depth = uview_depth_range.y - (fog_ndc * (uview_depth_range.y - uview_depth_range.x));
    float fog_base = uview_fog[1].x / (uview_fog[1].y - fog_depth);
    float fog_f = clamp(fog_base - uview_fog[1].z, 0.0f, 1.0f);
    float fog_z = fog_f;
    float3 fog_color = lerp(s2_color, uview_fog[0].xyz, clamp(fog_z, 0.0f, 1.0f).xxx);
    o_color = float4(fog_color, s2_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_tex0_uv = stage_input.in_tex0_uv;
    in_tex1_uv = stage_input.in_tex1_uv;
    in_tex2_uv = stage_input.in_tex2_uv;
    in_cc0 = stage_input.in_cc0;
    in_fog_zw = stage_input.in_fog_zw;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
