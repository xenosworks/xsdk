#version 450
// The atlas's red is the font's bit, so the colour where it's set and nothing where it isn't.
layout(location = 0) in vec2 v_atlas;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(push_constant) uniform Push { vec4 color; } pc;

void main()
{
	o_color = pc.color * texture(u_atlas, v_atlas).r;
}
