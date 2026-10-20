#version 450
// The texture times the sprite's colour.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	o_color = texture(u_texture, v_uv) * v_color;
}
