// D3D9's pixel semantics: interpolators read by semantic, out of order and with those between unread, and COLORn
// targets and DEPTH among a struct's members, declared out of order.
struct PS_OUT {
    float4 c1 : COLOR1;
    float depth : DEPTH;
    float4 c0 : COLOR0;
};

PS_OUT ps_main(float4 uv1 : TEXCOORD1, float4 spec : COLOR1, float fog : FOG, float4 colour : COLOR0)
{
    PS_OUT o;
    o.c1 = uv1 * fog;
    o.depth = colour.w * 0.5;
    o.c0 = colour + spec;
    return o;
}
