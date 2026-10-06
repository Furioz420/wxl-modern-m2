#include "wxl/RenderDefaults.hpp"
// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainShadows.hpp"
#include "wxl/RenderReloadKey.hpp"
#include "ShadowSettings.hpp"
#include "wxl/RenderControlApi.h"
#include "TerrainShadowPolicy.hpp"
#include "../ExtensionApi.hpp"
#include "engine/events/Event.hpp"
#include <wrl/client.h>
#include <unordered_map>
#include <cstdio>

namespace wxl::modern::terrainshadow
{
    namespace
    {
        const WXL_RenderControlApi* control=nullptr;
        constexpr wchar_t legacySettingsFile[]=L"wxl-native-shadows.ini";
        constexpr wchar_t settingsFile[]=L"wxl-native-shadows-v2.ini";
        shadowsettings::Tuning startup;
        const char* settingsNote="";
        template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
        struct Entry { Ptr<IDirect3DPixelShader9> original,enhanced; Contract contract{}; };
        struct State {
            bool enabled=false,failed=false;
            shadowsettings::Tuning tuning;
            unsigned matched=0,draws=0,objectDraws=0;
            IDirect3DDevice9* device=nullptr; // borrowed identity; shader references own their device
            std::unordered_map<IDirect3DPixelShader9*,Entry> shaders;
        };
        // COM objects are released on lifecycle callbacks, never by a DLL static destructor.
        State& Get() { static auto* state=new State; return *state; }
        void Lost(void*,const void*) { auto& s=Get(); s.shaders.clear(); s.device=nullptr; s.matched=0; s.draws=0; s.objectDraws=0; s.failed=false; }
        void OnReloadInput(void*,const void* args) {
            const auto* a=static_cast<const wxl::events::InputArgs*>(args);
            static wxl::render::ReloadKey key;
            if(!key.Handle(a->message,a->wparam,a->handled)) return;
            auto& s=Get(); const float old=s.tuning.strength;
            const auto result=shadowsettings::LoadPreferred(settingsFile,legacySettingsFile,s.tuning);
            settingsNote=result==shadowsettings::settings::Result::Ok ? "F12: saved shadows loaded." : shadowsettings::settings::Message(result);
            if(old!=s.tuning.strength) { s.shaders.clear(); s.matched=0; }
            WLOG_INFO("render-reload: shadows result=%u",unsigned(result));
        }
        void Panel(void*)
        {
            auto& s=Get(); auto* api=wxl_modern_m2::g_api;
            const float previousStrength=s.tuning.strength;
            api->UiText("World shadows: strength and softer edges");
            if(api->UiButton("Native")) s.tuning.enhanced=false;
            if(api->UiButton("Enhanced")) s.tuning.enhanced=true;
            api->UiText(s.tuning.enhanced ? "Selected: Enhanced" : "Selected: Native");
            if(api->UiSliderFloat) {
                api->UiSliderFloat("Shadow depth",&s.tuning.strength,.5f,2.f);
                api->UiSliderFloat("Edge softness",&s.tuning.softness,1.f,2.f);
            }
            if(api->UiButton("Accepted look")) s.tuning={true,1.35f,1.35f};
            api->UiText("Depth below 1.0 lightens shadows; 1.0 is native. Enhanced applies these controls.");
            if(api->UiButton("Save")) {
                const auto result=shadowsettings::Save(settingsFile,s.tuning);
                settingsNote=result==shadowsettings::settings::Result::Ok ? "Shadow selection saved." : shadowsettings::settings::Message(result);
            }
            if(api->UiButton("Reload")) {
                const auto result=shadowsettings::LoadPreferred(settingsFile,legacySettingsFile,s.tuning);
                settingsNote=result==shadowsettings::settings::Result::Ok ? "Saved shadow selection loaded." : shadowsettings::settings::Message(result);
            }
            if(api->UiButton("Restore startup")) { s.tuning=startup; settingsNote="Startup selection restored; saved file unchanged."; }
            // A strength change needs a new bytecode variant; softness only changes
            // scoped tap constants. Clear both positive and negative cache entries.
            if(s.tuning.strength!=previousStrength) { s.shaders.clear(); s.matched=0; }
            api->UiText(settingsNote);
            api->UiText("Save remembers your shadow selection for the next enhanced-renderer run.");
            if(!WxlRenderEffectsEnabled(control)) api->UiText("F11 comparison is bypassing enhancements; your selection is preserved.");
            char line[128]; std::snprintf(line,sizeof(line),"Matched shaders: %u; enhanced draws: %u; object draws: %u",s.matched,s.draws,s.objectDraws); api->UiText(line);
            if(!s.draws) api->UiText("Find visible outdoor ground shadows with native shadow quality enabled.");
            if(s.failed) api->UiText("Stopped after a device-state error; restart the test.");
        }
        const Entry* Variant(IDirect3DDevice9* d,IDirect3DPixelShader9* ps)
        {
            auto& s=Get();
            if(auto it=s.shaders.find(ps);it!=s.shaders.end()) return it->second.enhanced ? &it->second : nullptr;
            // Bound both memory and work; retaining originals prevents pointer-reuse cache errors.
            if(s.shaders.size()>=512) return nullptr;
            Entry entry; entry.original=ps;
            UINT bytes=0;
            if(SUCCEEDED(ps->GetFunction(nullptr,&bytes)) && bytes && bytes<=16384 && bytes%4==0)
            {
                std::vector<DWORD> words(bytes/4); UINT returned=bytes;
                if(SUCCEEDED(ps->GetFunction(words.data(),&returned)) && returned==bytes && Patch(words,&entry.contract,s.tuning.strength)
                    && SUCCEEDED(d->CreatePixelShader(words.data(),entry.enhanced.GetAddressOf())))
                { ++s.matched; WLOG_INFO("terrain-shadows: matched receiver bytes=%u family=%s",bytes,entry.contract.firstTap==3 ? "terrain" : "object"); }
            }
            auto [it,inserted]=s.shaders.emplace(ps,std::move(entry));
            return it->second.enhanced ? &it->second : nullptr;
        }
        bool ReceiverTarget(IDirect3DDevice9* d,const Contract& c)
        {
            Ptr<IDirect3DSurface9> target,back;
            if(FAILED(d->GetRenderTarget(0,target.GetAddressOf())) ||
                FAILED(d->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,back.GetAddressOf())) || target.Get()!=back.Get()) return false;
            UINT width=0,height=0;
            for(unsigned slot=c.firstSampler;slot<c.firstSampler+c.samplerCount;++slot)
            {
                Ptr<IDirect3DBaseTexture9> texture;
                if(FAILED(d->GetTexture(slot,texture.GetAddressOf())) || !texture || texture->GetType()!=D3DRTYPE_TEXTURE) return false;
                D3DSURFACE_DESC desc{};
                if(FAILED(static_cast<IDirect3DTexture9*>(texture.Get())->GetLevelDesc(0,&desc)) ||
                    desc.Format!=D3DFMT_D24X8 || !(desc.Usage&D3DUSAGE_DEPTHSTENCIL)) return false;
                if(slot==c.firstSampler) { width=desc.Width; height=desc.Height; }
                else if(width!=desc.Width || height!=desc.Height) return false;
            }
            return width && height;
        }
    }
    void Install()
    {
        if(wxl_modern_m2::g_api->GetInterface) control=static_cast<const WXL_RenderControlApi*>(
            wxl_modern_m2::g_api->GetInterface("wxl.render-control",WXL_RENDER_CONTROL_API_VERSION));
        auto& s=Get();
        s.enabled=wxl::render::defaults::Enabled("WXL_TERRAIN_SHADOWS");
        if(!s.enabled) return;
        const auto loaded=shadowsettings::LoadPreferred(settingsFile,legacySettingsFile,s.tuning); startup=s.tuning;
        WLOG_INFO("terrain-shadows: load settings result=%u enhanced=%u",static_cast<unsigned>(loaded),s.tuning.enhanced);
        auto* api=wxl_modern_m2::g_api;
        api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnDeviceLost),Lost,nullptr);
        api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnInput),OnReloadInput,nullptr);
        api->UiAddPanel("Native shadows",Panel,nullptr);
        WLOG_INFO("terrain-shadows: enabled; exact terrain/object receivers; strength=%.3f softness=%.3f",s.tuning.strength,s.tuning.softness);
    }
    long Draw(void* device,int type,int base,unsigned min,unsigned vertices,unsigned start,unsigned count,WXL_M2Draw_DIPFn original)
    {
        auto call=[&] { return original(device,type,base,min,vertices,start,count); };
        auto& s=Get(); if(!s.enabled || !s.tuning.enhanced || s.failed || !device || !count) return call();
        if(!WxlRenderEffectsEnabled(control)) return call();
        auto* d=static_cast<IDirect3DDevice9*>(device);
        if(s.device!=d) { Lost(nullptr,nullptr); s.device=d; }
        Ptr<IDirect3DPixelShader9> ps;
        if(FAILED(d->GetPixelShader(ps.GetAddressOf())) || !ps) return call();
        const Entry* enhanced=nullptr;
        try { enhanced=Variant(d,ps.Get()); } catch(...) { return call(); }
        if(!enhanced || !ReceiverTarget(d,enhanced->contract)) return call();
        const auto& c=enhanced->contract;
        float before[32],after[32];
        if(FAILED(d->GetPixelShaderConstantF(c.firstTap,before,8)) || !Soften(before,after,c.tapMask,s.tuning.softness)) return call();
        HRESULT constants=d->SetPixelShaderConstantF(c.firstTap,after,8);
        HRESULT shader=SUCCEEDED(constants) ? d->SetPixelShader(enhanced->enhanced.Get()) : E_FAIL;
        long result=S_OK;
        if(SUCCEEDED(constants) && SUCCEEDED(shader))
        {
            result=call();
            if(!s.draws) WLOG_INFO("terrain-shadows: first enhanced terrain draw; verified maps and offsets");
            ++s.draws;
            if(c.firstTap!=3 && !s.objectDraws++) WLOG_INFO("terrain-shadows: first enhanced object receiver draw; verified maps and offsets");
        }
        // Restore even after a setup/draw failure; do not leak into water, WMO, UI or the next draw.
        HRESULT restoreShader=d->SetPixelShader(ps.Get());
        HRESULT restoreConstants=d->SetPixelShaderConstantF(c.firstTap,before,8);
        if(FAILED(restoreShader) || FAILED(restoreConstants))
        {
            s.failed=true; WLOG_WARN("terrain-shadows: state restoration failed; disabled");
            return FAILED(restoreShader) ? restoreShader : restoreConstants;
        }
        if(FAILED(constants) || FAILED(shader)) return call();
        return result;
    }
}
