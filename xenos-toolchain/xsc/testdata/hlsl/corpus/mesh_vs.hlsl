uniform row_major float4x4 pc_mvp : register(c0);


static float4 gl_Position;
static float3 a_position;
static float2 v_uv;
static float2 a_uv;

struct SPIRV_Cross_Input
{
    float3 a_position : TEXCOORD0;
    float2 a_uv : TEXCOORD1;
};

struct SPIRV_Cross_Output
{
    float2 v_uv : TEXCOORD0;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    gl_Position = mul(float4(a_position, 1.0f), pc_mvp);
    v_uv = a_uv;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    a_position = stage_input.a_position;
    a_uv = stage_input.a_uv;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.v_uv = v_uv;
    return stage_output;
}
