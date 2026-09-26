// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Used by the default-off live layer adapter. Requires the extended particle VS.
// PS constants: c0=(colorMultiplier, alphaMultiplier, alphaCutoff, blendAddMaterial), c2=native fog RGB.
// Alpha cutoff and multipliers require explicit verified material inputs, not guessed source flags.
sampler2D layer0 : register(s0);
sampler2D layer1 : register(s1);
sampler2D layer2 : register(s2);
float4 parameters : register(c0);
float4 fogColor : register(c2);
struct Input {
    float4 color : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float fog : FOG;
};
float4 Finish(float4 c, float fog) {
    c *= float4(parameters.xxx, parameters.y);
    clip(c.a-parameters.z);
    // BlendAdd's neutral fog RGB is black; retain the native distance/visibility function.
    // No extra RGB-by-alpha multiplication: source RGB and opacity remain independent.
    float3 target = parameters.w > 0.5 ? float3(0,0,0) : fogColor.rgb;
    c.rgb = (c.rgb-target)*fog+target;
    return c;
}
float4 TwoColorThreeAlpha(Input i) : COLOR0 {
    float4 a=tex2D(layer0,i.uv0), b=tex2D(layer1,i.uv1), c=tex2D(layer2,i.uv2);
    if(parameters.w > 0.5) clip(a.a-parameters.z);
    return Finish(float4(i.color.rgb*a.rgb*b.rgb, i.color.a*a.a*b.a*c.a),i.fog);
}
float4 ThreeColorThreeAlpha(Input i) : COLOR0 {
    float4 a=tex2D(layer0,i.uv0);
    if(parameters.w > 0.5) clip(a.a-parameters.z);
    return Finish(i.color*a*tex2D(layer1,i.uv1)*tex2D(layer2,i.uv2),i.fog);
}
