#version 450
// psize_vs.hlsl's twin.
layout(set = 0, binding = 0) uniform Globals { vec4 Size; } g;
layout(location = 0) in vec4 pos;

void main()
{
    gl_Position = pos;
    gl_PointSize = g.Size.x * pos.w;
}
