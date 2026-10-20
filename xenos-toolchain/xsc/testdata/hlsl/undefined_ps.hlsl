// A vector given two of its components, the others left undefined (OpUndef), and a product with 0, which glslang
// folds to a null constant (OpConstantNull).
float4 K : register(c0);

struct PS_OUT {
    float4 partial : COLOR0;
    float4 zero : COLOR1;
};

PS_OUT ps_main(float2 uv : TEXCOORD0)
{
    PS_OUT o;
    float4 r;
    r.xy = uv;
    o.partial = r * K;
    o.zero = K * 0.0;
    return o;
}
