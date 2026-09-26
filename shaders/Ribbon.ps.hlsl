// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Only the explicitly unlit/unfogged alpha-blended source subset is supported.
// UV equation follows the pinned WebWowViewerCpp ribbon shader.
sampler2D ribbonTexture : register(s0);
float4 scaleOffset : register(c0);
float4 SourceUV(float4 color : COLOR0,float2 uv : TEXCOORD0) : COLOR0 {
    return color*tex2D(ribbonTexture,uv*scaleOffset.xy+scaleOffset.zw);
}
// Deliberately opaque diagnostic color, not a material/tint fix.
float4 Marker() : COLOR0 {return float4(1,0,1,1);}

sampler2D ribbonTexture1 : register(s1);
sampler2D ribbonTexture2 : register(s2);
float4 scaleOffset1 : register(c1);
float4 scaleOffset2 : register(c2);
// Experimental 3Color/3Alpha candidate: no tint, intensity boost, or altered framebuffer blend.
// Three-map product is established for particle programs, NOT yet proven for donor ribbons.
float4 ThreeMapCandidate(float4 color : COLOR0,float2 uv : TEXCOORD0) : COLOR0 {
    return color*tex2D(ribbonTexture,uv*scaleOffset.xy+scaleOffset.zw)
        *tex2D(ribbonTexture1,uv*scaleOffset1.xy+scaleOffset1.zw)
        *tex2D(ribbonTexture2,uv*scaleOffset2.xy+scaleOffset2.zw);
}
