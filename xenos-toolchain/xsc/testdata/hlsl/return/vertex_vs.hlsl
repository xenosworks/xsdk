// An early return in a vertex shader: the position set before it, COLOR0 by it or after it.
float4 Scale : register(c0);

struct Output {
    float4 position : POSITION;
    float4 colour : COLOR0;
};

Output vs_main(float4 position : POSITION, float4 colour : COLOR0)
{
    Output o;
    o.position = position * Scale;
    o.colour = colour;
    if (position.x > 0.5) {
        o.colour = float4(1.0, 0.0, 0.25, 1.0);
        return o;
    }
    o.colour.g *= 0.5;
    return o;
}
