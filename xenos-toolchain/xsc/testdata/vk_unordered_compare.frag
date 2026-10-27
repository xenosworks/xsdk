#version 450
// Float !=, which glslang makes OpFUnordNotEqual, as PPSSPP's alpha test does (God of War: Chains of Olympus,
// `if (roundAndScaleTo255f(v.a) != u_alphacolorref.a) discard;`): a discard on one, and colours from two more.
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform Ref { vec4 ref; } u;

float roundAndScaleTo255f(float x)
{
	return floor(x * 255.0 + 0.5);
}

void main()
{
	if (roundAndScaleTo255f(v_color.a) != u.ref.a)
		discard;
	o_color = vec4(v_color.r != u.ref.r ? 1.0 : 0.25, v_color.g != u.ref.g ? 1.0 : 0.25, 0.0, 1.0);
}
