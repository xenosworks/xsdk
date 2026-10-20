// The grid's DEPTH: TEXCOORD0's x, 0 at the quad's left to 1 at its right, the colour green.
struct PS_OUT {
    float4 colour : COLOR0;
    float depth : DEPTH;
};

PS_OUT ps_main(float2 uv : TEXCOORD0)
{
    PS_OUT o;
    o.colour = float4(0.0, 1.0, 0.0, 1.0);
    o.depth = uv.x;
    return o;
}
