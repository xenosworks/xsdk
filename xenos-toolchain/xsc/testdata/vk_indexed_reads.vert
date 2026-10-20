#version 450
// Rows read at run-time indices, which the optimiser reads where they're used only while a0 holds their index
// (xsdk:src/xsc_optimize.c, same_values): offsets[j] read at j and tints[i] at i, then used past m[j]'s load of 4j and
// i's; tints[i] used again past the interpolators' alloc, which loses a0, before any load; scales[j] and offsets[i]
// read past it and used at once.
layout(location = 0) in vec4 a_vertex;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_other;
layout(set = 0, binding = 0) uniform Rows { mat4 m[4]; vec4 tints[16]; vec4 offsets[16]; vec4 scales[16]; } u;

void main()
{
    int i = int(a_vertex.z);
    int j = int(a_vertex.w);
    vec4 offset = u.offsets[j];
    vec4 pos = u.m[j] * vec4(a_vertex.xy, 0.5, 1.0);
    vec4 tint = u.tints[i];
    gl_Position = pos + offset * tint;
    v_color = tint * 2.0 + u.scales[j] * offset;
    v_other = u.offsets[i];
}
