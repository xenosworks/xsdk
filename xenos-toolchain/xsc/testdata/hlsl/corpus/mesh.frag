#version 450
// The texture times the tint in the fragment range of the push constants.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { layout(offset = 64) vec4 tint; } pc;

void main()
{
	o_color = texture(u_texture, v_uv) * pc.tint;
}
