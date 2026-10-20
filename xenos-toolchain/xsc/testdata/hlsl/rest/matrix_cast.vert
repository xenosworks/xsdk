#version 450
// matrix_cast_vs.hlsl's twin: mat3() of a column-major mat4, the same registers read as columns.
layout(set = 0, binding = 0) uniform Globals { mat4 World; mat4 WorldViewProj; vec4 LightDir; } g;
layout(location = 0) in vec4 pos;
layout(location = 1) in vec3 n;
layout(location = 0) out vec3 on;
layout(location = 1) out vec4 d;

void main()
{
    gl_Position = pos * g.WorldViewProj;
    vec3 nn = n * mat3(g.World);
    on = nn;
    d = vec4(-g.LightDir.xyz, dot(nn, -g.LightDir.xyz));
}
