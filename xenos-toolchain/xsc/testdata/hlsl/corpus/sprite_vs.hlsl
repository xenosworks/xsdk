
static float4 gl_Position;
static float2 a_position;
static float2 v_uv;
static float2 a_uv;
static float4 v_color;
static float4 a_color;

struct SPIRV_Cross_Input
{
    float2 a_position : TEXCOORD0;
    float2 a_uv : TEXCOORD1;
    float4 a_color : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float2 v_uv : TEXCOORD0;
    float4 v_color : TEXCOORD1;
    float4 gl_Position : POSITION;
};

void vert_main()
{
    gl_Position = float4(a_position, 0.5f, 1.0f);
    v_uv = a_uv;
    v_color = a_color;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    a_position = stage_input.a_position;
    a_uv = stage_input.a_uv;
    a_color = stage_input.a_color;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.v_uv = v_uv;
    stage_output.v_color = v_color;
    return stage_output;
}
