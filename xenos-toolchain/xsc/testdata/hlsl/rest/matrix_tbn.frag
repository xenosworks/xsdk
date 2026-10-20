#version 450
// matrix_tbn_ps.hlsl's twin: the frame's rows as interpolators of their own, made a mat3.
layout(location = 0) in vec3 v;
layout(location = 1) in vec3 t;
layout(location = 2) in vec3 b;
layout(location = 3) in vec3 n;
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(mat3(t, b, n) * v, 1.0);
}
