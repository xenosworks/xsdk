#version 450
// The texture's luminance times the tint in the fragment range of the push constants.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(push_constant) uniform Push { layout(offset = 64) vec4 tint; } pc;

void main()
{
	vec4 c = texture(u_texture, v_uv);
	float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
	o_color = vec4(l, l, l, c.a) * pc.tint;
}
