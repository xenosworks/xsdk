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

uniform sampler2D tex0;
uniform sampler2D fog_range_table;

static float4 gl_FragCoord;
static float2 in_tex0_uv;
static float4 in_cc0;
static float2 in_fog_zw;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float4 in_cc0 : TEXCOORD0;
    float2 in_tex0_uv : TEXCOORD1;
    float2 in_fog_zw : TEXCOORD2;
    float4 gl_FragCoord : VPOS;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float2 tex0_uv = in_tex0_uv;
    float4 sampled0 = tex2D(tex0, tex0_uv);
    float3 s0_color = clamp(0.0f.xxx + umat_tevreg[2].xyz, 0.0f.xxx, 1.0f.xxx);
    float s0_alpha = clamp(lerp(0.0f, in_cc0.w, sampled0.w) + 0.0f, 0.0f, 1.0f);
    float3 s1_color = clamp(0.0f.xxx + s0_color, (-4.0f).xxx, 4.0f.xxx);
    float s1_alpha = clamp(lerp(0.0f, umat_kcolor[3].w, s0_alpha) + 0.0f, -4.0f, 4.0f);
    float fog_ndc = in_fog_zw.x / in_fog_zw.y;
    float fog_depth = uview_depth_range.y - (fog_ndc * (uview_depth_range.y - uview_depth_range.x));
    float fog_base = uview_fog[1].x / (uview_fog[1].y - fog_depth);
    float fog_column = gl_FragCoord.x * 0.015625f;
    float4 fog_range = tex2D(fog_range_table, float2(fog_column, (floor(fog_column) * 0.03125f) + 0.015625f));
    float fog_f = clamp((fog_base * fog_range.x) - uview_fog[1].z, 0.0f, 1.0f);
    float fog_z = fog_f;
    float3 fog_color = lerp(s1_color, uview_fog[0].xyz, clamp(fog_z, 0.0f, 1.0f).xxx);
    float3 out_color = ((fog_color * 255.0f) - (floor((fog_color * 255.0f) / 256.0f.xxx) * 256.0f)) / 255.0f.xxx;
    float out_alpha = ((s1_alpha * 255.0f) - (floor((s1_alpha * 255.0f) / 256.0f) * 256.0f)) / 255.0f;
    o_color = float4(out_color, out_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord + float4(0.5f, 0.5f, 0.0f, 0.0f);
    in_tex0_uv = stage_input.in_tex0_uv;
    in_cc0 = stage_input.in_cc0;
    in_fog_zw = stage_input.in_fog_zw;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
