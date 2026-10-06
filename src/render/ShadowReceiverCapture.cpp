// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include "ShadowReceiverCapture.hpp"
#include "../ExtensionApi.hpp"
#include "engine/events/Event.hpp"
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <fstream>
#include <filesystem>

namespace wxl::modern::shadowcapture
{
    namespace
    {
        constexpr unsigned kFrames=240,kPerFrame=64,kShaders=128,kByteLimit=16384;
        bool enabled=false,armed=false,automaticPending=false;
        unsigned sequence=0;
        unsigned frame=0,reads=0,selected=0,sampled=0,casters=0,skipped=0,failed=0;
        std::filesystem::path directory;
        std::vector<std::vector<unsigned char>> shaders;
        template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
        template<class T> bool Bytecode(T* shader,std::vector<unsigned char>& bytes)
        {
            UINT length=0;
            if(!shader || FAILED(shader->GetFunction(nullptr,&length)) || !length || length>kByteLimit) return false;
            bytes.resize(length); UINT returned=length;
            return SUCCEEDED(shader->GetFunction(bytes.data(),&returned)) && returned==length;
        }
        bool Write(const std::filesystem::path& path,const std::vector<unsigned char>& bytes)
        {
            std::ofstream file(path,std::ios::binary); if(!file) return false;
            file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            file.close(); return !file.fail();
        }
        void Stop(const char* reason)
        {
            if(!armed) return; armed=false;
            WLOG_INFO("shadow-receiver-capture: stopped=%s frames=%u shaders=%u skippedDraws=%u failures=%u folder=%s",
                reason,frame,static_cast<unsigned>(shaders.size()),skipped,failed,directory.string().c_str());
            std::ofstream summary(directory/"summary.txt");
            summary<<"reason="<<reason<<"\nframes="<<frame<<"\nuniquePixelShaders="<<shaders.size()
                <<"\nskippedDraws="<<skipped<<"\nfailures="<<failed<<"\nsampledReads="<<sampled<<"\ncastersSkipped="<<casters
                <<"\nCoverage: rotating modulo-97 sampling over the entire indexed frame; exclude known casters and non-backbuffer draws; max 64 reads/frame, 240 frames, 128 shaders."
                <<"\nPre-interceptor observations; not proof of receiver identity or coverage of non-indexed draws.\n";
            shaders.clear();
        }
        void Begin()
        {
            automaticPending=false;
            directory=std::filesystem::path("Logs")/"WxlShadowCapture"/
                (std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(++sequence));
            std::error_code error;
            if(!std::filesystem::create_directories(directory,error) || error)
            { WLOG_WARN("shadow-receiver-capture: cannot create output folder"); return; }
            frame=reads=selected=sampled=casters=skipped=failed=0; shaders.clear(); armed=true;
            WLOG_INFO("shadow-receiver-capture: armed; up to %u frames, %u shaders; folder=%s",kFrames,kShaders,directory.string().c_str());
        }
        void Input(void*,const void* args)
        {
            const auto& a=*static_cast<const wxl::events::InputArgs*>(args);
            if(!enabled || (a.message!=WM_KEYDOWN && a.message!=WM_SYSKEYDOWN) ||
                a.wparam!=VK_F10 || (a.lparam&(1u<<30))) return;
            if(a.handled) *a.handled=true;
            if(armed) Stop("manual"); else Begin();
        }
        void World(void*,const void*) { if(automaticPending && !armed) Begin(); }
        void Frame(void*,const void*) { reads=selected=0; if(armed && ++frame>=kFrames) Stop("frame-budget"); }
        void Lost(void*,const void*) { Stop("device-lost"); }
    }
    void Install()
    {
        char value[8]{};
        enabled=GetEnvironmentVariableA("WXL_SHADOW_RECEIVER_CAPTURE",value,sizeof(value))==1 && value[0]=='1';
        if(!enabled) return;
        automaticPending=true;
        wxl_modern_m2::g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnInput),Input,nullptr);
        wxl_modern_m2::g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnFrame),Frame,nullptr);
        wxl_modern_m2::g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnDeviceLost),Lost,nullptr);
        wxl_modern_m2::g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnWorldRenderEnd),World,nullptr);
        WLOG_INFO("shadow-receiver-capture: automatic capture at first world frame; F10 starts/stops a bounded read-only indexed-draw capture");
    }
    void Observe(void* device,unsigned route,unsigned vertices,unsigned primitives)
    {
        if(!armed || !device) return;
        // Sample across the entire frame, not a prefix dominated by thousands of caster draws.
        const unsigned index=reads++;
        if(route&8u) { ++casters; return; }
        if(index%97!=frame%97 || selected>=kPerFrame) { ++skipped; return; }
        auto* d=static_cast<IDirect3DDevice9*>(device);
        Ptr<IDirect3DSurface9> target,back;
        if(FAILED(d->GetRenderTarget(0,target.GetAddressOf())) ||
            FAILED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf())) || target.Get()!=back.Get())
        { ++skipped; return; }
        ++selected; ++sampled;
        Ptr<IDirect3DPixelShader9> pixel;
        if(FAILED(d->GetPixelShader(pixel.GetAddressOf())) || !pixel) return;
        std::vector<unsigned char> code;
        if(!Bytecode(pixel.Get(),code)) { ++failed; return; }
        for(const auto& existing:shaders) if(existing==code) return;
        const auto stem="shader-"+std::to_string(shaders.size());
        if(!Write(directory/(stem+".ps.bin"),code)) { ++failed; Stop("write-failed"); return; }
        shaders.push_back(std::move(code));
        Ptr<IDirect3DVertexShader9> vertex;
        if(SUCCEEDED(d->GetVertexShader(vertex.GetAddressOf())) && Bytecode(vertex.Get(),code))
            if(!Write(directory/(stem+".vs.bin"),code)) ++failed;
        std::ofstream info(directory/(stem+".txt"));
        info<<"frame="<<frame<<"\ndrawOrdinal="<<index<<"\nroute="<<route<<"\nvertices="<<vertices<<"\nprimitives="<<primitives
            <<"\nrouteBits: 1=one-shot armed, 2=ribbon, 4=M2 context, 8=shadow caster context\n";
        for(unsigned stage=0;stage<16;++stage)
        {
            Ptr<IDirect3DBaseTexture9> texture;
            if(FAILED(d->GetTexture(stage,texture.GetAddressOf())) || !texture) continue;
            info<<"sampler="<<stage<<" type="<<texture->GetType();
            if(texture->GetType()==D3DRTYPE_TEXTURE)
            {
                D3DSURFACE_DESC desc{};
                if(SUCCEEDED(static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0,&desc)))
                    info<<" format="<<static_cast<unsigned>(desc.Format)<<" width="<<desc.Width<<" height="<<desc.Height<<" usage="<<desc.Usage;
            }
            info<<'\n';
        }
        float constants[32][4]{};
        const auto dump=[&](const char* label) {
            for(unsigned i=0;i<32;++i) info<<label<<i<<'='<<constants[i][0]<<','<<constants[i][1]<<','<<constants[i][2]<<','<<constants[i][3]<<'\n';
        };
        if(SUCCEEDED(d->GetPixelShaderConstantF(0,constants[0],32))) dump("ps.c");
        if(SUCCEEDED(d->GetVertexShaderConstantF(0,constants[0],32))) dump("vs.c");
        info.close(); if(info.fail()) ++failed;
        if(shaders.size()>=kShaders) Stop("shader-budget");
    }
}
