#version 450
// asin, acos and atan of a vector, each lane its own, and asin and acos of a float (tests/test_inverse_trig.py).
layout(location = 0) in vec4 x;
layout(location = 1) in float y;
layout(location = 0) out vec4 o_asin;
layout(location = 1) out vec4 o_acos;
layout(location = 2) out vec4 o_scalar;
layout(location = 3) out vec4 o_atan;

void main() {
    o_asin = asin(x);
    o_acos = acos(x);
    o_scalar = vec4(asin(y), acos(y), 0.0, 0.0);
    o_atan = atan(x);
}
