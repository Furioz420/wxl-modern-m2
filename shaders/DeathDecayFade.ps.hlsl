// Mod_Add alpha correction for the captured unlit Death and Decay meshes.
// Native RGB, UV and fog equations retained; fade applies to both map alphas.
sampler2D firstMap : register(s0);
sampler2D secondMap : register(s1);
float4 fogColour : register(c2);
float4 main(float4 colour : COLOR0, float2 uv0 : TEXCOORD0,
            float2 uv1 : TEXCOORD1, float fog : FOG) : COLOR0 {
    float4 a=tex2D(firstMap,uv0), b=tex2D(secondMap,uv1);
    return float4(lerp(fogColour.rgb,colour.rgb*a.rgb+b.rgb,fog),
                  colour.a*(a.a+b.a));
}
