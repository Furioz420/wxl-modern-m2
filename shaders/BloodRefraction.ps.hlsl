// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Bounded height-gradient approximation, NOT the retail refraction pass.
// This source texture stores a height/mask in R, not an RG normal vector.
sampler2D heightMap : register(s0);
sampler2D sceneColor : register(s1);
// xy = inverse target size; z = gradient gain; w = maximum displacement in pixels.
float4 scale : register(c0);
float4 main(float4 color : COLOR0, float2 uv : TEXCOORD0, float2 pixel : VPOS) : COLOR0 {
    float height = tex2D(heightMap, uv).r;
    float2 gradient = float2(ddx(height), ddy(height));
    float2 offset = clamp(gradient * scale.z, -scale.ww, scale.ww) * saturate(color.a);
    float2 screenUV = (pixel + 0.5) * scale.xy;
    float2 sampleUV = clamp(screenUV + offset * scale.xy, 0.5 * scale.xy, 1.0 - 0.5 * scale.xy);
    // RGB comes only from the current scene. RT alpha is preserved by the color mask.
    return float4(tex2D(sceneColor, sampleUV).rgb, 1.0);
}
