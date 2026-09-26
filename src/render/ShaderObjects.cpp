// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "ShaderObjects.hpp"
#include "ShaderCache.hpp"
#include "../ExtensionApi.hpp"
namespace wxl::modern::shaderobjects {
namespace {
    Cache<IDirect3DDevice9,IDirect3DVertexShader9,4> vertices;
    Cache<IDirect3DDevice9,IDirect3DPixelShader9,6> pixels;
    SRWLOCK lock=SRWLOCK_INIT;
    struct Locked {Locked(){AcquireSRWLockExclusive(&lock);}~Locked(){ReleaseSRWLockExclusive(&lock);}};
    bool enabled=false,subscribed=false,suspended=false;
    struct HeldDevice {
        IDirect3DDevice9* value=nullptr;
        ~HeldDevice(){Set(nullptr);}
        void Set(IDirect3DDevice9* d){if(d)d->AddRef();if(value)value->Release();value=d;}
    } lostDevice;
    unsigned reports=0;
    void Clear(const char* reason) {
        WLOG_INFO("m2-shader-cache: clear reason=%s vsLive=%u psLive=%u vsCreated=%llu psCreated=%llu vsHits=%llu psHits=%llu failures=%llu overflow=%llu",
            reason,vertices.Count(),pixels.Count(),vertices.created,pixels.created,vertices.hits,pixels.hits,
            vertices.failures+pixels.failures,vertices.overflow+pixels.overflow);
        vertices.Clear();pixels.Clear();
    }
    void Device(IDirect3DDevice9* d) {
        if((vertices.Owner()&&vertices.Owner()!=d)||(pixels.Owner()&&pixels.Owner()!=d))Clear("device-change");
    }
    void Report() {
        if(reports<12)WLOG_INFO("m2-shader-cache: sample=%u vsLive=%u psLive=%u vsCreated=%llu psCreated=%llu vsHits=%llu psHits=%llu failures=%llu overflow=%llu",
            ++reports,vertices.Count(),pixels.Count(),vertices.created,pixels.created,vertices.hits,pixels.hits,
            vertices.failures+pixels.failures,vertices.overflow+pixels.overflow);
    }
    void __cdecl Lost(void*,const void* args) {Locked guard;if(!enabled)return;
        const auto* event=static_cast<const wxl::events::DeviceResetArgs*>(args);
        lostDevice.Set(event?static_cast<IDirect3DDevice9*>(event->device):(vertices.Owner()?vertices.Owner():pixels.Owner()));
        suspended=true;Clear("device-lost");}
    void __cdecl Reset(void*,const void*) {Locked guard;if(!enabled)return;Clear("device-reset");lostDevice.Set(nullptr);suspended=false;}
    void __cdecl Leave(void*,const void*) {Locked guard;if(enabled)Clear("world-leave");}
    bool Ready(IDirect3DDevice9* d) {
        if(!suspended)return true;
        if(!d||!lostDevice.value||d==lostDevice.value)return false;
        // A newly created device may replace a lost device without a Reset event.
        // Strong ownership of the old device prevents pointer-reuse ambiguity.
        lostDevice.Set(nullptr);suspended=false;return true;
    }
}
void Initialize() {
    Locked guard;
    enabled=wxl_modern_m2::ConfigBool("WXL_M2_SHADER_OBJECT_CACHE",false);
    if(!enabled){vertices.Clear();pixels.Clear();lostDevice.Set(nullptr);suspended=false;return;}
    if(!wxl_modern_m2::g_api||!wxl_modern_m2::g_api->Subscribe){enabled=false;return;}
    if(!subscribed){
        using wxl::events::Event;
        wxl_modern_m2::g_api->Subscribe(unsigned(Event::OnDeviceLost),&Lost,nullptr);
        wxl_modern_m2::g_api->Subscribe(unsigned(Event::OnDeviceReset),&Reset,nullptr);
        wxl_modern_m2::g_api->Subscribe(unsigned(Event::OnWorldLeave),&Leave,nullptr);
        subscribed=true;
    }
    WLOG_INFO("m2-shader-cache: enabled maxVS=4 maxPS=6; immutable programs only; clear on device loss/change and world leave; geometry/textures/state blocks remain draw-local");
}
HRESULT Vertex(IDirect3DDevice9* d,const DWORD* code,IDirect3DVertexShader9** out) {
    Locked guard;
    if(!enabled)return d?d->CreateVertexShader(code,out):D3DERR_INVALIDCALL;
    if(!Ready(d))return D3DERR_DEVICELOST;
    if(!d)return D3DERR_INVALIDCALL;Device(d);
    const auto hr=vertices.Acquire(d,code,out,[](auto* device,auto* bytes,auto** value){return device->CreateVertexShader(bytes,value);});
    Report();return hr;
}
HRESULT Pixel(IDirect3DDevice9* d,const DWORD* code,IDirect3DPixelShader9** out) {
    Locked guard;
    if(!enabled)return d?d->CreatePixelShader(code,out):D3DERR_INVALIDCALL;
    if(!Ready(d))return D3DERR_DEVICELOST;
    if(!d)return D3DERR_INVALIDCALL;Device(d);
    const auto hr=pixels.Acquire(d,code,out,[](auto* device,auto* bytes,auto** value){return device->CreatePixelShader(bytes,value);});
    Report();return hr;
}
}
