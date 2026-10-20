#version 450
layout(push_constant, std140) uniform Push {
    vec4 color;
} upush;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = upush.color;
}
