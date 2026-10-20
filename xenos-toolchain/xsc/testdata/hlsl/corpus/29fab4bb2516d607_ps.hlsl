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

uniform sampler2D tex2;
uniform sampler2D tex0;
uniform sampler2D tex3;

static float3 in_tex0_uvw;
static float2 in_tex1_uv;
static float2 in_tex2_uv;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float3 in_tex0_uvw : TEXCOORD0;
    float2 in_tex1_uv : TEXCOORD1;
    float2 in_tex2_uv : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float2 tex0_uv = in_tex0_uvw.xy / in_tex0_uvw.z.xx;
    float2 tex1_uv = in_tex1_uv;
    float2 tex2_uv = in_tex2_uv;
    float3 ind_tex0 = tex2D(tex2, ((tex1_uv * umat_texcoord_scale[1].xy) * 1.0f.xx) / umat_tex_size_bias[2].xy).wzy * 255.0f;
    float3 ind0_coord = ind_tex0;
    float3 ind0_biased = ind0_coord + (-128.0f).xxx;
    float4 ind0_c0 = uview_ind_mtx[0];
    float4 ind0_c1 = uview_ind_mtx[1];
    float2 ind0_texel = tex0_uv * umat_texcoord_scale[0].xy;
    float2 tev_coord0 = ind0_texel + (float2(dot(float3(ind0_c0.xz, ind0_c1.x), ind0_biased), dot(float3(ind0_c0.yw, ind0_c1.y), ind0_biased)) * ind0_c1.z);
    float2 ind0_uv = tev_coord0 / umat_tex_size_bias[0].xy;
    float4 sampled0 = tex2D(tex0, ind0_uv);
    float4 sampled1 = tex2D(tex3, tex2_uv);
    float3 s0_color = clamp(lerp(0.0f.xxx, sampled0.xyz, 1.0f.xxx) + 0.0f.xxx, 0.0f.xxx, 1.0f.xxx);
    float s0_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s1_color = clamp(lerp(0.0f.xxx, sampled1.xyz, s0_color) + 0.0f.xxx, 0.0f.xxx, 1.0f.xxx);
    float s1_alpha = clamp(lerp(0.0f, sampled1.w, s0_alpha) + 0.0f, 0.0f, 1.0f);
    o_color = float4(s1_color, s1_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_tex0_uvw = stage_input.in_tex0_uvw;
    in_tex1_uv = stage_input.in_tex1_uv;
    in_tex2_uv = stage_input.in_tex2_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
