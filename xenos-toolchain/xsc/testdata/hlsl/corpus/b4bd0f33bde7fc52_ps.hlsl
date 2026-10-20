uniform float4 umat_tevreg[4] : register(c0);
uniform float4 umat_kcolor[4] : register(c4);
uniform float4 umat_texcoord_scale[8] : register(c8);
uniform float4 umat_tex_size_bias[8] : register(c16);

uniform sampler2D tex0;

static float2 in_tex0_uv;
static float4 in_cc0;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float4 in_cc0 : TEXCOORD0;
    float2 in_tex0_uv : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float2 tex0_uv = in_tex0_uv;
    float4 sampled0 = tex2D(tex0, tex0_uv);
    float3 s0_color = clamp((lerp(0.0f.xxx, sampled0.xyz, in_cc0.xyz) + 0.0f.xxx) * 4.0f, 0.0f.xxx, 1.0f.xxx);
    float s0_alpha = clamp(lerp(0.0f, in_cc0.w, sampled0.w) + 0.0f, 0.0f, 1.0f);
    float3 s1_color = clamp(lerp(umat_tevreg[1].xyz, 0.0f.xxx, in_cc0.w.xxx) + s0_color, 0.0f.xxx, 1.0f.xxx);
    float s1_alpha = clamp(0.0f + s0_alpha, 0.0f, 1.0f);
    float alpha_byte = floor((s1_alpha * 255.0f) + 0.5f);
    float alpha_pass = step(-0.5f, alpha_byte - 128.0f);
    if (alpha_pass < 0.5f)
    {
        clip(-1.0f);
    }
    o_color = float4(s1_color, s1_alpha);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_tex0_uv = stage_input.in_tex0_uv;
    in_cc0 = stage_input.in_cc0;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
