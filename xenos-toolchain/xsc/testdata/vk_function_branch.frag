#version 450
// A helper that returns early, which xost doesn't inline and refuses (tests/test_functions.py).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float pick(float x)
{
	if (x > 0.5)
		return 1.0;
	return x;
}

void main()
{
	o_color = vec4(pick(v_uv.x));
}
