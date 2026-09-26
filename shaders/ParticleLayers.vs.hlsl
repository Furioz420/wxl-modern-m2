// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Native transform/lighting/fog contract recovered from the September 5 particle capture.
// Independent UV1/UV2 MUST be produced per particle; forwarding UV0 again is not supported.
float4 projectionRows[4] : register(c2);
float4 uvRows[2] : register(c6);
float4 lightDiffuse : register(c10);
float4 lightAmbient : register(c11);
float4 lightDirection : register(c12);
float4 pointColor : register(c17);
float4 secondPointColor : register(c18);
float4 pointPosition : register(c21);
float4 secondPointPosition : register(c22);
float4 attenuationConstant : register(c25);
float4 attenuationLinear : register(c26);
float4 attenuationQuadratic : register(c27);
float4 colorBias : register(c29);
float4 fogParameters : register(c30);
float4 modelRows[3] : register(c31);
struct Input {
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    float4 color : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
};
struct Output {
    float4 position : POSITION0;
    float4 color : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float fog : FOG;
};
float3 TransformPosition(float3 v) {
    float4 p=float4(v,1);
    return float3(dot(modelRows[0],p),dot(modelRows[1],p),dot(modelRows[2],p));
}
float3 TransformNormal(float3 n) {
    return normalize(float3(dot(modelRows[0].xyz,n),dot(modelRows[1].xyz,n),dot(modelRows[2].xyz,n)));
}
Output Common(Input i, float3 p, float4 color) {
    Output o;
    float4 v=float4(p,1);
    o.position=float4(dot(projectionRows[0],v),dot(projectionRows[1],v),dot(projectionRows[2],v),dot(projectionRows[3],v));
    o.fog=min(pow(max(p.z*fogParameters.x+fogParameters.y,0),fogParameters.z),1);
    o.color=color;
    o.uv0=float2(dot(uvRows[0].xyw,float3(i.uv0,1)),dot(uvRows[1].xyw,float3(i.uv0,1)));
    o.uv1=i.uv1;
    o.uv2=i.uv2;
    return o;
}
float3 DirectionalLight(float3 n) {
    return saturate(dot(-lightDirection.xyz,n))*lightDiffuse.rgb+lightAmbient.rgb;
}
Output Unlit(Input i) { return Common(i,TransformPosition(i.position),i.color); }
Output Directional(Input i) {
    float3 light=saturate(DirectionalLight(TransformNormal(i.normal)));
    return Common(i,TransformPosition(i.position),saturate(i.color*float4(light,1)+colorBias));
}
Output DirectionalPoint(Input i) {
    float3 p=TransformPosition(i.position), n=TransformNormal(i.normal), d=pointPosition.xyz-p;
    float distance2=dot(d,d), invDistance=rsqrt(distance2);
    float attenuation=rcp(attenuationConstant.x+distance2*invDistance*attenuationLinear.x+distance2*attenuationQuadratic.x);
    float pointAmount=attenuation*max(dot(d,n)*invDistance,0);
    float3 light=saturate(DirectionalLight(n)+pointColor.rgb*pointAmount);
    return Common(i,p,saturate(i.color*float4(light,1)+colorBias));
}
// Exact transform/UV/fog and two-point lighting ABI from the test-09 812-byte VS.
// Attenuation coefficients are packed per light in x/y of c25..27.
Output DirectionalTwoPoints(Input i) {
    float3 p=TransformPosition(i.position), n=TransformNormal(i.normal);
    float3 d0=pointPosition.xyz-p, d1=secondPointPosition.xyz-p;
    float2 distance2=float2(dot(d0,d0),dot(d1,d1));
    float2 invDistance=rsqrt(distance2);
    float2 attenuation=rcp(attenuationConstant.xy+distance2*invDistance*attenuationLinear.xy+distance2*attenuationQuadratic.xy);
    float2 amount=attenuation*max(float2(dot(d0,n),dot(d1,n))*invDistance,0);
    float3 light=saturate(DirectionalLight(n)+pointColor.rgb*amount.x+secondPointColor.rgb*amount.y);
    return Common(i,p,saturate(i.color*float4(light,1)+colorBias));
}
