
static float4 gl_Position;
static float2 in_position;

struct SPIRV_Cross_Input
{
    float2 in_position : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 gl_Position : POSITION;
};

void vert_main()
{
    gl_Position = float4(in_position, 0.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    in_position = stage_input.in_position;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    return stage_output;
}
