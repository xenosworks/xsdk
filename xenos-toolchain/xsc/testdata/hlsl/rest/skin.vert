#version 450
// skin_vs.hlsl's twin: the bones row-major mat4x3s, 3 registers each, D3DCOLORtoUBYTE4 written out.
layout(set = 0, binding = 0, row_major) uniform Globals { mat4x3 Bones[8]; } g;
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 weights;
layout(location = 2) in vec4 indices;

void main()
{
    ivec4 idx = ivec4(indices.zyxw * 255.001953);
    vec3 p = g.Bones[idx.x] * pos * weights.x;
    p += g.Bones[idx.y] * pos * weights.y;
    p += g.Bones[idx.z] * pos * weights.z;
    p += g.Bones[idx.w] * pos * (1.0 - weights.x - weights.y - weights.z);
    gl_Position = vec4(p, 1.0);
}
