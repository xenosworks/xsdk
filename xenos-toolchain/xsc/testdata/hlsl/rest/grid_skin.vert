#version 450
// hlsl/grid_skin_vs.hlsl's twin: the bones row-major mat4x3s, the indices an ivec4 input.
layout(set = 0, binding = 0, row_major) uniform Globals { mat4x3 Bones[8]; } g;
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 weights;
layout(location = 2) in ivec4 indices;
layout(location = 3) in vec2 uv;
layout(location = 0) out vec4 colour;
layout(location = 1) out vec2 ouv;

void main()
{
    vec3 p = g.Bones[indices.x] * pos * weights.x;
    p += g.Bones[indices.y] * pos * weights.y;
    p += g.Bones[indices.z] * pos * weights.z;
    p += g.Bones[indices.w] * pos * (1.0 - weights.x - weights.y - weights.z);
    gl_Position = vec4(p.xy, 0.5, 1.0);
    colour = vec4(p.z, 0.75, 0.0, 1.0);
    ouv = uv;
}
