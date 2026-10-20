#version 450
// The texture times the tint in a second dynamic uniform buffer.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 1, binding = 1) uniform Tint { vec4 tint; } u_tint;

void main()
{
	o_color = texture(u_texture, v_uv) * u_tint.tint;
}
