uniform float4 umat_tevreg[4] : register(c0);
uniform float4 umat_kcolor[4] : register(c4);
uniform float4 umat_texcoord_scale[8] : register(c8);
uniform float4 umat_tex_size_bias[8] : register(c16);

uniform sampler2D tex0;

static float2 in_tex0_uv;
static float2 in_tex1_uv;
static float2 in_tex2_uv;
static float2 in_tex3_uv;
static float2 in_tex4_uv;
static float2 in_tex5_uv;
static float2 in_tex6_uv;
static float2 in_tex7_uv;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float2 in_tex0_uv : TEXCOORD0;
    float2 in_tex1_uv : TEXCOORD1;
    float2 in_tex2_uv : TEXCOORD2;
    float2 in_tex3_uv : TEXCOORD3;
    float2 in_tex4_uv : TEXCOORD4;
    float2 in_tex5_uv : TEXCOORD5;
    float2 in_tex6_uv : TEXCOORD6;
    float2 in_tex7_uv : TEXCOORD7;
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
    float2 tex3_uv = in_tex3_uv;
    float2 tex4_uv = in_tex4_uv;
    float2 tex5_uv = in_tex5_uv;
    float2 tex6_uv = in_tex6_uv;
    float2 tex7_uv = in_tex7_uv;
    float4 sampled0 = tex2D(tex0, tex0_uv);
    float4 sampled1 = tex2D(tex0, tex1_uv);
    float4 sampled2 = tex2D(tex0, tex2_uv);
    float4 sampled3 = tex2D(tex0, tex3_uv);
    float4 sampled4 = tex2D(tex0, tex4_uv);
    float4 sampled5 = tex2D(tex0, tex5_uv);
    float4 sampled6 = tex2D(tex0, tex6_uv);
    float4 sampled7 = tex2D(tex0, tex7_uv);
    float3 s0_color = clamp(lerp(0.0f.xxx, sampled0.xyz, umat_tevreg[2].w.xxx) + 0.0f.xxx, 0.0f.xxx, 1.0f.xxx);
    float s0_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s1_color = clamp(lerp(0.0f.xxx, sampled1.xyz, umat_tevreg[2].w.xxx) + s0_color, 0.0f.xxx, 1.0f.xxx);
    float s1_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s2_color = clamp(lerp(0.0f.xxx, sampled2.xyz, umat_tevreg[2].w.xxx) + s1_color, 0.0f.xxx, 1.0f.xxx);
    float s2_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s3_color = clamp(lerp(0.0f.xxx, sampled3.xyz, umat_tevreg[2].w.xxx) + s2_color, 0.0f.xxx, 1.0f.xxx);
    float s3_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s4_color = clamp(lerp(0.0f.xxx, sampled4.xyz, umat_tevreg[2].w.xxx) + s3_color, 0.0f.xxx, 1.0f.xxx);
    float s4_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s5_color = clamp(lerp(0.0f.xxx, sampled5.xyz, umat_tevreg[2].w.xxx) + s4_color, 0.0f.xxx, 1.0f.xxx);
    float s5_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s6_color = clamp(lerp(0.0f.xxx, sampled6.xyz, umat_tevreg[2].w.xxx) + s5_color, 0.0f.xxx, 1.0f.xxx);
    float s6_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    float3 s7_color = clamp(lerp(0.0f.xxx, sampled7.xyz, umat_tevreg[2].w.xxx) + s6_color, 0.0f.xxx, 1.0f.xxx);
    float s7_alpha = clamp(0.0f + umat_tevreg[1].w, 0.0f, 1.0f);
    o_color = float4(s7_color, s7_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_tex0_uv = stage_input.in_tex0_uv;
    in_tex1_uv = stage_input.in_tex1_uv;
    in_tex2_uv = stage_input.in_tex2_uv;
    in_tex3_uv = stage_input.in_tex3_uv;
    in_tex4_uv = stage_input.in_tex4_uv;
    in_tex5_uv = stage_input.in_tex5_uv;
    in_tex6_uv = stage_input.in_tex6_uv;
    in_tex7_uv = stage_input.in_tex7_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
