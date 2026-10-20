uniform float4 umat_tevreg[4] : register(c0);
uniform float4 umat_kcolor[4] : register(c4);
uniform float4 umat_texcoord_scale[8] : register(c8);
uniform float4 umat_tex_size_bias[8] : register(c16);

uniform sampler2D tex1;
uniform sampler2D tex2;
uniform sampler2D tex0;

static float2 in_tex0_uv;
static float2 in_tex1_uv;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float2 in_tex0_uv : TEXCOORD0;
    float2 in_tex1_uv : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float2 tex0_uv = in_tex0_uv;
    float2 tex1_uv = in_tex1_uv;
    float4 sampled0 = tex2D(tex1, tex1_uv);
    float4 sampled1 = tex2D(tex2, tex1_uv);
    float4 sampled2 = tex2D(tex0, tex0_uv);
    float3 s0_color = clamp(lerp(0.0f.xxx, sampled0.xyz, umat_kcolor[0].xyz) + umat_tevreg[1].xyz, (-4.0f).xxx, 4.0f.xxx);
    float s0_alpha = clamp(umat_tevreg[1].w - lerp(0.0f, sampled0.w, umat_kcolor[0].w), -4.0f, 4.0f);
    float3 s1_color = clamp((lerp(0.0f.xxx, sampled1.xyz, umat_kcolor[1].xyz) + s0_color) * 2.0f, (-4.0f).xxx, 4.0f.xxx);
    float s1_alpha = clamp(s0_alpha - lerp(0.0f, sampled1.w, umat_kcolor[1].w), -4.0f, 4.0f);
    float3 s2_color = clamp(lerp(0.0f.xxx, sampled2.xyz, 1.0f.xxx) + s1_color, 0.0f.xxx, 1.0f.xxx);
    float s2_alpha = clamp(lerp(sampled2.w, 0.0f, 0.0f) + s1_alpha, 0.0f, 1.0f);
    float3 s3_color = clamp(lerp(s2_alpha.xxx, s2_color, umat_kcolor[2].xyz) + 0.0f.xxx, 0.0f.xxx, 1.0f.xxx);
    float s3_alpha = 0.0f;
    o_color = float4(s3_color, s3_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_tex0_uv = stage_input.in_tex0_uv;
    in_tex1_uv = stage_input.in_tex1_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
