// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <array>
#include <cstdint>
#include <d3d9.h>
namespace wxl::modern::shaderobjects {
// Keys MUST be immutable, process-lifetime compiled-in program arrays. Never use
// model memory or temporary bytecode. Caller serializes cache/lifecycle access.
template<class Device,class Shader,size_t Capacity> class Cache {
    struct Entry {const DWORD* code=nullptr;Shader* shader=nullptr;};
    std::array<Entry,Capacity> entries_{};
    Device* device_=nullptr;
public:
    uint64_t hits=0,created=0,overflow=0,failures=0;
    Cache()=default;Cache(const Cache&)=delete;Cache& operator=(const Cache&)=delete;
    ~Cache(){Clear();}
    Device* Owner() const noexcept{return device_;}
    unsigned Count() const noexcept{unsigned n=0;for(const auto& e:entries_)if(e.shader)++n;return n;}
    void Clear() noexcept {
        for(auto& e:entries_){if(e.shader)e.shader->Release();e={};}
        if(device_){device_->Release();device_=nullptr;}
    }
    template<class Create> HRESULT Acquire(Device* d,const DWORD* code,Shader** out,Create create) {
        if(!d||!code||!out||*out)return D3DERR_INVALIDCALL;
        if(device_!=d){Clear();device_=d;device_->AddRef();}
        Entry* free=nullptr;
        for(auto& e:entries_){
            if(e.shader&&e.code==code){e.shader->AddRef();*out=e.shader;++hits;return D3D_OK;}
            if(!e.shader&&!free)free=&e;
        }
        Shader* made=nullptr;const HRESULT hr=create(d,code,&made);
        if(FAILED(hr)||!made){if(made)made->Release();++failures;return FAILED(hr)?hr:E_FAIL;}
        ++created;
        if(free){*free={code,made};made->AddRef();} // cache + independent draw reference
        else ++overflow; // bounded cache full: ordinary uncached draw, no eviction hazard
        *out=made;return hr;
    }
};
}
