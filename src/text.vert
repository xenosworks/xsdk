#version 450
// A glyph's corner: where it sits, in pixels from the viewport's top left, and where its cell is in the atlas.
layout(location = 0) in vec2 a_position;
layout(location = 2) in vec2 a_atlas;
layout(location = 0) out vec2 v_atlas;
// Pixels to clip space: scaled by xy, then moved by zw.
layout(push_constant) uniform Push { vec4 to_clip; } pc;

void main()
{
	gl_Position = vec4(a_position * pc.to_clip.xy + pc.to_clip.zw, 0.0, 1.0);
	v_atlas = a_atlas;
}
