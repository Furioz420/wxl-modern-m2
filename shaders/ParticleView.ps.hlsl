// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// DIAGNOSTIC ONLY. Preserve full composite opacity/UV coverage, display one RGB input.
// No claim of independent layer blending or donor material appearance.
sampler2D layer0 : register(s0);
sampler2D layer1 : register(s1);
sampler2D layer2 : register(s2);
float4 parameters : register(c0); // normal EXP2 multipliers and cutoff; RGB multiplier deliberately unused
float4 diagnostic : register(c1); // x: 1=vertex RGB, 2=layer0 RGB, 3=layer1 RGB, 4=layer2 RGB
struct Input {float4 color:COLOR0;float2 uv0:TEXCOORD0;float2 uv1:TEXCOORD1;float2 uv2:TEXCOORD2;float fog:FOG;};
float4 LayerView(Input i):COLOR0 {
    float4 a=tex2D(layer0,i.uv0),b=tex2D(layer1,i.uv1),c=tex2D(layer2,i.uv2);
    clip(a.a-parameters.z);
    float alpha=i.color.a*a.a*b.a*c.a*parameters.y;
    clip(alpha-parameters.z);
    float3 rgb=i.color.rgb;
    if(diagnostic.x>1.5)rgb=a.rgb;
    if(diagnostic.x>2.5)rgb=b.rgb;
    if(diagnostic.x>3.5)rgb=c.rgb;
    return float4(rgb*i.fog,alpha);
}
