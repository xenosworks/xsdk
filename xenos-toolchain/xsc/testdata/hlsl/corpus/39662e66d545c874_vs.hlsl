uniform float4 upush_uv_transform : register(c0);


static float4 gl_Position;
static float2 in_position;
static float2 out_uv;
static float2 in_uv;

struct SPIRV_Cross_Input
{
    float2 in_position : TEXCOORD0;
    float2 in_uv : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float2 out_uv : TEXCOORD0;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    gl_Position = float4(in_position, 0.0f, 1.0f);
    out_uv = (in_uv * upush_uv_transform.zw) + upush_uv_transform.xy;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_position = stage_input.in_position;
    in_uv = stage_input.in_uv;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.out_uv = out_uv;
    return stage_output;
}
