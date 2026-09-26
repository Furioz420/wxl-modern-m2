// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <d3d9.h>
namespace wxl::modern::drawtrace {
    // Capture complete Present-to-Present intervals after target activity, not arbitrary
    // successful-draw samples. Unsigned subtraction preserves GetTickCount wrap semantics.
    struct Schedule {
        unsigned used=0;
        uint32_t last=0;
        bool hasLast=false;
        bool Next(uint32_t now,bool targetSeen) noexcept {
            if(!targetSeen||used>=8||(hasLast&&uint32_t(now-last)<500))return false;
            last=now;hasLast=true;++used;return true;
        }
    };
    template<class T>struct Ref {T* p=nullptr;~Ref(){if(p)p->Release();}};
    struct Snapshot {
        uintptr_t rt=0,rtTexture=0,backbuffer=0,texture[3]{},vs=0,ps=0;
        D3DSURFACE_DESC desc{};
        D3DVIEWPORT9 viewport{};
        DWORD colorWrite=0,zWrite=0,zEnable=0;
        unsigned valid=0;
    };
    // Read-only. No retained COM references, readback, resource allocation or state setter.
    // Bitmask distinguishes failed reads from genuine null bindings/zero values.
    inline Snapshot Read(IDirect3DDevice9* device) noexcept {
        Snapshot s;
        if(!device)return s;
        Ref<IDirect3DSurface9> rt,bb;
        if(SUCCEEDED(device->GetRenderTarget(0,&rt.p))&&rt.p){
            s.valid|=1;s.rt=reinterpret_cast<uintptr_t>(rt.p);
            if(SUCCEEDED(rt.p->GetDesc(&s.desc)))s.valid|=2;
            Ref<IDirect3DTexture9> container;
            if(SUCCEEDED(rt.p->GetContainer(__uuidof(IDirect3DTexture9),reinterpret_cast<void**>(&container.p)))&&container.p)
                {s.valid|=4;s.rtTexture=reinterpret_cast<uintptr_t>(container.p);}
        }
        if(SUCCEEDED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb.p))&&bb.p)
            {s.valid|=8;s.backbuffer=reinterpret_cast<uintptr_t>(bb.p);}
        if(SUCCEEDED(device->GetViewport(&s.viewport)))s.valid|=16;
        for(unsigned n=0;n<3;++n){Ref<IDirect3DBaseTexture9> t;
            if(SUCCEEDED(device->GetTexture(n,&t.p))){s.valid|=32u<<n;s.texture[n]=reinterpret_cast<uintptr_t>(t.p);}}
        Ref<IDirect3DVertexShader9> vs;Ref<IDirect3DPixelShader9> ps;
        if(SUCCEEDED(device->GetVertexShader(&vs.p))){s.valid|=256;s.vs=reinterpret_cast<uintptr_t>(vs.p);}
        if(SUCCEEDED(device->GetPixelShader(&ps.p))){s.valid|=512;s.ps=reinterpret_cast<uintptr_t>(ps.p);}
        if(SUCCEEDED(device->GetRenderState(D3DRS_COLORWRITEENABLE,&s.colorWrite)))s.valid|=1024;
        if(SUCCEEDED(device->GetRenderState(D3DRS_ZWRITEENABLE,&s.zWrite)))s.valid|=2048;
        if(SUCCEEDED(device->GetRenderState(D3DRS_ZENABLE,&s.zEnable)))s.valid|=4096;
        return s;
    }
}
