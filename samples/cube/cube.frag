#version 450
// The face's colour times the texture.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
	o_color = vec4(v_color, 1.0) * texture(u_texture, v_uv);
}
