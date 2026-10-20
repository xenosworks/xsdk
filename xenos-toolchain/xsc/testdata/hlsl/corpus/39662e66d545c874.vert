#version 450
layout(push_constant, std140) uniform Push {
    vec4 uv_transform;
} upush;
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 out_uv;
void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    out_uv = in_uv * upush.uv_transform.zw + upush.uv_transform.xy;
}
