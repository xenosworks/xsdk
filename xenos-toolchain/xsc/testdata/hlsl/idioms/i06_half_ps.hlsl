// half types and h-suffixed literals, as SM3 code writes for partial precision.
sampler2D Tex : register(s0);
half4 Tint : register(c0);

half4 ps_main(half2 uv : TEXCOORD0, half4 col : COLOR0) : COLOR0
{
    half4 t = tex2D(Tex, uv);
    half3 c = t.rgb * col.rgb * Tint.rgb;
    half l = dot(c, half3(0.299h, 0.587h, 0.114h));
    return half4(lerp(c, l.xxx, 0.5h), t.a * col.a);
}
