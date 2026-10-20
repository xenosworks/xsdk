#version 450
// slots_ps.hlsl's GLSL twin, its samplers at Vulkan bindings 1, 4, 0 and 2: GLSL's still pack from 0 in binding order.
layout(set = 0, binding = 1) uniform sampler2D A;
layout(set = 0, binding = 4) uniform sampler2D B;
layout(set = 0, binding = 0) uniform sampler2D C;
layout(set = 0, binding = 2) uniform sampler2D D;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main()
{
    colour = vec4(texture(A, uv).x, texture(B, uv).y, texture(C, uv).z, texture(D, uv).w);
}
