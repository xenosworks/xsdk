#version 450
// Clamps and a wrap a value's range makes useless, dropped, and clamps it doesn't, kept (tests/test_ranges.py).
layout(location = 0) in vec4 v_a;
layout(location = 1) in vec4 v_b;
layout(location = 0) out vec4 o_dropped;
layout(location = 1) out vec4 o_wrap;
layout(location = 2) out vec4 o_kept;
layout(location = 3) out vec4 o_edges;

void main()
{
	vec4 a = clamp(v_a, 0.0, 1.0);
	vec4 p = a * clamp(v_b * 0.5, 0.0, 1.0);
	// A ±4 clamp of a product of saturated values and a clamp of that, and a min and a max of a, in 0 to 1.
	o_dropped = clamp(clamp(p, -4.0, 4.0), 0.0, 1.0) + max(min(a, 1.0), 0.0);
	// A wrap at 256 of p * 255, whose floor is 0.
	o_wrap = (p * 255.0 - floor(p * 255.0 / 256.0) * 256.0) / 255.0;
	// A clamp of an input; a min of a value that may be NaN; a max of -a and a, -0 where a is +0, and its clamp, +0 there,
	// which 1 / x shows.
	vec4 w = max(vec4(-1.0), min(vec4(0.5), v_b));
	o_kept = clamp(v_a, -4.0, 4.0) + min(w, 4.0) + 1.0 / clamp(max(vec4(0.0) - a, a), 0.0, 1.0);
	// Kept, each biting at its operand's top: a's floor, 1 at 1; a min of p * 2 with 1; the floor of a product whose
	// exact top is below 1 but rounds to 1. Dropped: a min of a * 2 with its top, 2.
	o_edges = floor(a) * 256.0 + min(p * 2.0, 1.0) + floor(a * 0.14285715 * 6.9999995) * 16.0 + min(a * 2.0, 2.0);
}
