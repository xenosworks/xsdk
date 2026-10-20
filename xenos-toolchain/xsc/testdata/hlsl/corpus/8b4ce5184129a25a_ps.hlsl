uniform float4 upush_uv_transform : register(c0);
uniform float4 upush_palette_scale : register(c1);

uniform sampler2D source;
uniform sampler2D palette;

static float2 in_uv;
static float4 o_color;

struct SPIRV_Cross_Input
{
    float2 in_uv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 o_color : COLOR0;
};

void frag_main()
{
    float4 index = floor((tex2D(source, in_uv) * 15.0f) + 0.5f.xxxx);
    o_color = tex2D(palette, (index.xx * upush_palette_scale.xy) + upush_palette_scale.zw);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_uv = stage_input.in_uv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.o_color = float4(o_color);
    return stage_output;
}
