// Scoped trial: preserve the third map's authored RGB as well as coverage.
// Not recovered retail TXAC semantics. Fade and native blend stay unchanged.
sampler2D firstMap : register(s0);
sampler2D secondMap : register(s1);
sampler2D maskMap : register(s2);
float4 fogColour : register(c2);
float4 main(float4 colour : COLOR0,float2 uv0:TEXCOORD0,float2 uv1:TEXCOORD1,
            float2 maskUV:TEXCOORD2,float fog:FOG):COLOR0 {
    float4 a=tex2D(firstMap,uv0),b=tex2D(secondMap,uv1),detail=tex2D(maskMap,maskUV);
    return float4(lerp(fogColour.rgb,colour.rgb*(a.rgb+b.rgb)*detail.rgb,fog),
                  colour.a*(a.a+b.a)*detail.a);
}
