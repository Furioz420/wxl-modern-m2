// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "ParticleRefraction.hpp"
#include "ParticleNativeShaderPolicy.hpp"
#include "ParticleDiagnosticPolicy.hpp"
#include "../ExtensionApi.hpp"
#include "engine/render/SceneColorCopy.hpp"
#include <wrl/client.h>
#include "shaders/BloodRefractionPs.h"

namespace wxl::modern::particlelayers {
namespace {
using Microsoft::WRL::ComPtr;
struct Prepared {
    ComPtr<IDirect3DStateBlock9> state;
    ComPtr<IDirect3DPixelShader9> shader;
    wxl::render::SceneColorCopy<IDirect3DDevice9,IDirect3DTexture9,IDirect3DSurface9> copy;
    bool captured=false;
    HRESULT error=S_OK;
    const char* reason="draw-shape";
    bool Restore() {if(!captured)return true;captured=false;return SUCCEEDED(state->Apply());}
    ~Prepared(){Restore();}
    bool Check(HRESULT hr,const char* why){error=hr;reason=why;return SUCCEEDED(hr);}
    bool Prepare(IDirect3DDevice9* d) {
        ComPtr<IDirect3DVertexShader9> vs;ComPtr<IDirect3DPixelShader9> ps;
        ComPtr<IDirect3DBaseTexture9> map;ComPtr<IDirect3DVertexBuffer9> vb;
        UINT offset=0,stride=0;DWORD zwrite=1,srgbRead=1,srgbWrite=1,colorWrite=0;
        particlediag::ShaderBytes bytes;
        if(!Check(d->GetVertexShader(vs.GetAddressOf()),"vertex-shader") || !vs ||
           !particlediag::ReadShader(vs.Get(),bytes) ||
           IdentifyNativeVertex(bytes.data.data(),bytes.size)!=NativeVertexKind::Unlit)return false;
        if(!Check(d->GetPixelShader(ps.GetAddressOf()),"pixel-shader") || !ps ||
           !particlediag::ReadShader(ps.Get(),bytes) || bytes.size!=sizeof(programs::NativePixel) ||
           std::memcmp(bytes.data.data(),programs::NativePixel,sizeof(programs::NativePixel)))return false;
        if(!Check(d->GetStreamSource(0,vb.GetAddressOf(),&offset,&stride),"stream") || !vb || stride!=24 ||
           !Check(d->GetTexture(0,map.GetAddressOf()),"height-map") || !map ||
           !Check(d->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite),"depth-write") || zwrite ||
           !Check(d->GetSamplerState(0,D3DSAMP_SRGBTEXTURE,&srgbRead),"srgb-read") || srgbRead ||
           !Check(d->GetRenderState(D3DRS_SRGBWRITEENABLE,&srgbWrite),"srgb-write") || srgbWrite ||
           !Check(d->GetRenderState(D3DRS_COLORWRITEENABLE,&colorWrite),"color-write"))return false;
        if(!Check(d->CreateStateBlock(D3DSBT_ALL,state.GetAddressOf()),"state-block") ||
           !Check(state->Capture(),"state-capture"))return false;
        captured=true;
        WXL_SceneColor color{};color.structSize=sizeof(color);
        // Draw-local ownership: no stale frame, reset, resize or world-leave resources.
        // Copy the ACTIVE target immediately before this emitter, even without liquid.
        if(!copy.Capture(d,color)){error=copy.Error();reason="current-scene-copy";return false;}
        if(!Check(d->CreatePixelShader(reinterpret_cast<const DWORD*>(kBloodRefractionShader),shader.GetAddressOf()),"create-shader"))return false;
        const float parameters[]{1.0f/color.width,1.0f/color.height,64.0f,4.0f};
        if(!Check(d->SetPixelShader(shader.Get()),"bind-shader") ||
           !Check(d->SetPixelShaderConstantF(0,parameters,1),"parameters") ||
           !Check(d->SetTexture(1,static_cast<IDirect3DBaseTexture9*>(static_cast<IDirect3DTexture9*>(color.texture))),"scene-bind"))return false;
        for(auto address:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})if(!Check(d->SetSamplerState(1,address,D3DTADDRESS_CLAMP),"scene-address"))return false;
        for(auto filter:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})if(!Check(d->SetSamplerState(1,filter,D3DTEXF_LINEAR),"scene-filter"))return false;
        if(!Check(d->SetSamplerState(1,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"scene-mips") ||
           !Check(d->SetSamplerState(1,D3DSAMP_SRGBTEXTURE,0),"scene-srgb") ||
           !Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"replace-color") ||
           !Check(d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE),"alpha-test") ||
           !Check(d->SetRenderState(D3DRS_FOGENABLE,FALSE),"scene-already-fogged") ||
           !Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,colorWrite&(D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE)),"rgb-only"))return false;
        return true;
    }
};
}
long DrawBloodRefraction(void* device,int type,int base,unsigned min,unsigned vertices,
    unsigned start,unsigned primitives,DIPFn original) {
    Prepared prepared;bool ready=false;
    if(device && type==D3DPT_TRIANGLELIST && RefractionQuadBatch(vertices,primitives))
        ready=prepared.Prepare(static_cast<IDirect3DDevice9*>(device));
    // Successful startup draws must not consume the fallback report budget.
    static unsigned appliedReports=0,fallbackReports=0;
    if(!ready) {
        const bool restored=prepared.Restore();
        if(fallbackReports<8){++fallbackReports;WLOG_WARN("m2-blood-refraction: fallback reason=%s hr=%#lx restored=%u type=%d vertices=%u primitives=%u base=%d min=%u start=%u; native draw retained",prepared.reason,prepared.error,unsigned(restored),type,vertices,primitives,base,min,start);}
        return restored?original(device,type,base,min,vertices,start,primitives):D3DERR_INVALIDCALL;
    }
    const long result=original(device,type,base,min,vertices,start,primitives);
    const bool restored=prepared.Restore();
    if(appliedReports<8){++appliedReports;WLOG_INFO("m2-blood-refraction: applied=1 currentScene=1 heightGradient=1 maxPixels=4 restored=%u hr=%#lx vertices=%u primitives=%u; approximation",unsigned(restored),result,vertices,primitives);}
    return restored?result:D3DERR_INVALIDCALL;
}
}
