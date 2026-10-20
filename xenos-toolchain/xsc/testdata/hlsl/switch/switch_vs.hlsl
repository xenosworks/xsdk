// A switch in a vertex shader, on an int from the position's x, without a default.
struct Output {
    float4 position : POSITION;
    float4 colour : COLOR0;
};

Output vs_main(float4 position : POSITION, float4 colour : COLOR0)
{
    Output o;
    o.position = position;
    o.colour = colour;
    switch ((int)(position.x * 4.0)) {
    case 1:
        o.colour.r = 0.25;
        break;
    case 2:
        o.colour.g = 0.5;
        break;
    }
    return o;
}
