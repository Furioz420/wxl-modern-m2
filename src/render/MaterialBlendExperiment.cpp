// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "MaterialBlendExperiment.hpp"
#include "MaterialBlendPolicy.hpp"
#include "DeathDecayFadePolicy.hpp"
#include "MaskedMeshPolicy.hpp"
#include "ParticleDiagnosticPolicy.hpp"
#include "ShaderObjects.hpp"
#include "shaders/DeathDecayNative.hpp"
#include "shaders/DeathDecayFadePs.h"
#include "shaders/DeathDecayColourPs.h"
#include "shaders/DeathDecayMaskPs.h"
#include "shaders/DeathDecayDetailPs.h"
#include "shaders/DeathDecayMaskVs.hpp"
#include "offsets/engine/Gx.hpp"
#include "../compat/MaterialConfig.hpp"
#include "../ExtensionApi.hpp"
#include "../compat/ModernM2.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"

namespace wxl::modern::materialblend
{
    namespace off = wxl::offsets::game::m2;
    namespace mat = assets::m2::material;
    namespace
    {
        bool enabled = false;
        bool deathDecayFade = false;
        bool deathDecayOmit = false;
        bool deathDecayColour = false;
        bool deathDecayMask = false;
        bool deathDecayDetail = false;
        bool maskedMeshMaterials = false;
        unsigned fadeReports = 0;
        unsigned reports = 0;
        struct Context
        {
            void* shared = nullptr;
            const void* owner = nullptr;
            const void* skin = nullptr;
            const void* batches = nullptr;
            uint32_t count = 0, index = 0;
            uint16_t blend = 0;
            uint16_t maskIndex = 0;
            wxl::structure::m2::M2Batch batch{};
            char path[276]{};
        };
        bool ReadContext(void* raw, void* instance, Context& out) noexcept
        {
            if (!raw || !instance) return false;
            __try
            {
                const auto* dc = static_cast<const off::DrawContext*>(raw);
                if (dc->instance != instance || !dc->element || !dc->material) return false;
                out.shared = reinterpret_cast<void*>(static_cast<off::M2Instance*>(instance)->model);
                if (!out.shared) return false;
                wxl::game::m2::M2Model model(out.shared);
                if(deathDecayFade || deathDecayOmit || deathDecayColour || (deathDecayMask && !maskedMeshMaterials)) {
                    const char* path=model.GetPathStem();
                    if(!path)return false;
                    size_t n=0;for(;n+1<sizeof(out.path)&&path[n];++n)out.path[n]=path[n];
                    if(path[n])return false;
                }
                const auto* skin = model.GetSkin();
                out.owner = model.GetHeader();
                if (!out.owner || !skin || !skin->batches || !skin->batchCount || skin->batchCount > 0x10000)
                    return false;
                out.index = *reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(dc->element) + off::kOffElementBatchIndex);
                if (out.index >= skin->batchCount) return false;
                out.skin = skin;
                out.batches = skin->batches;
                out.count = skin->batchCount;
                out.batch = skin->batches[out.index];
                out.blend = static_cast<const off::Material*>(dc->material)->blend;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool Select(void* raw, void* instance, Context& context) noexcept
        {
            try
            {
                if (!ReadContext(raw, instance, context) || context.blend != 4 ||
                    !assets::m2::IsNativeLoaded(context.shared)) return false;
                const auto source = mat::SourceMaterials().Find(context.owner, context.skin,
                    context.batches, context.count);
                return source && Eligible(*source, context.index, context.batch, context.blend);
            }
            catch (...) { return false; }
        }
        template<class T> struct ComRead
        {
            T* value = nullptr;
            ~ComRead() { if (value) value->Release(); }
        };
        bool HasProgrammableTextures(IDirect3DDevice9* device) noexcept
        {
            ComRead<IDirect3DVertexShader9> vs;
            ComRead<IDirect3DPixelShader9> ps;
            ComRead<IDirect3DBaseTexture9> t0, t1;
            return SUCCEEDED(device->GetVertexShader(&vs.value)) && vs.value &&
                SUCCEEDED(device->GetPixelShader(&ps.value)) && ps.value &&
                SUCCEEDED(device->GetTexture(0, &t0.value)) && t0.value &&
                SUCCEEDED(device->GetTexture(1, &t1.value)) && t1.value;
        }
        bool SelectFade(void* raw,void* instance,Context& context) noexcept {
            if(!deathDecayFade && !deathDecayOmit && !deathDecayColour && !deathDecayMask)return false;
            try {
                if(!ReadContext(raw,instance,context)||!assets::m2::IsNativeLoaded(context.shared))return false;
                auto source=mat::SourceMaterials().Find(context.owner,context.skin,context.batches,context.count);
                if(maskedMeshMaterials)return source&&MaskedMeshEligible(true,*source,context.index,context.batch,context.blend,context.maskIndex);
                if(!source||!DeathDecayFadeEligible(true,context.path,*source,context.index,context.batch,context.blend))return false;
                if(deathDecayMask) {
                    const auto& m=*source->model;
                    const auto& raw=source->batches[source->outputs[context.index].sourceBatch];
                    if(m.meshTxacState!=mat::MeshTxacState::Captured || raw.materialIndex>=m.meshTxac.size() ||
                       m.meshTxac[raw.materialIndex]!=std::array<uint8_t,2>{1,1})return false;
                    context.maskIndex=m.textureCombos[raw.textureComboIndex+2];
                    const unsigned transform=unsigned(raw.textureTransformComboIndex)+2;
                    if(transform>=m.transformCombos.size() || m.transformCombos[transform]!=0xffff ||
                       m.textures[context.maskIndex].flags!=4)return false;
                }
                return true;
            } catch(...){return false;}
        }
        template<class T,size_t N> bool ExactShader(T* shader,const uint32_t(&expected)[N]) {
            particlediag::ShaderBytes bytes;
            return shader&&particlediag::ReadShader(shader,bytes)&&bytes.size==sizeof(expected)&&
                   !std::memcmp(bytes.data.data(),expected,sizeof(expected));
        }
        IDirect3DBaseTexture9* ResolveMask(const Context& c) noexcept {
            __try {
                const auto* header=static_cast<const wxl::structure::m2::M2Header*>(c.owner);
                if(!header||c.maskIndex>=header->textures.count)return nullptr;
                auto** handles=*reinterpret_cast<void***>(static_cast<uint8_t*>(c.shared)+0x174);
                if(!handles||!handles[c.maskIndex])return nullptr;
                void* wrapper=reinterpret_cast<off::M2_TexResolveFn>(off::kTexResolve)(handles[c.maskIndex],0,0);
                void* gx=*reinterpret_cast<void**>(wxl::offsets::engine::gx::kGxDevicePtr);
                if(!wrapper||!gx)return nullptr;
                using Ready=void(__fastcall*)(void*,void*,void*);
                reinterpret_cast<Ready>((*reinterpret_cast<void***>(gx))[0])(gx,nullptr,wrapper);
                auto* texture=*reinterpret_cast<IDirect3DBaseTexture9**>(static_cast<uint8_t*>(wrapper)+0x38);
                if(texture && texture->GetType()==D3DRTYPE_TEXTURE){texture->AddRef();return texture;}
                return nullptr;
            } __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
        }
        bool DrawFade(IDirect3DDevice9* device,const Context& context,int type,int base,
            unsigned min,unsigned vertices,unsigned start,unsigned primitives,DrawFn original,long& result) {
            ComRead<IDirect3DVertexShader9> vs,maskVS;ComRead<IDirect3DPixelShader9> ps,replacement;
            ComRead<IDirect3DBaseTexture9> maskTexture;
            ComRead<IDirect3DStateBlock9> state;
            DWORD src=0,dst=0,on=0,op=0,zwrite=1;
            if(FAILED(device->GetVertexShader(&vs.value))||FAILED(device->GetPixelShader(&ps.value))||
               !ExactShader(vs.value,kDeathDecayNativeVS)||!ExactShader(ps.value,kDeathDecayNativePS)||
               !HasProgrammableTextures(device)||
               FAILED(device->GetRenderState(D3DRS_SRCBLEND,&src))||src!=D3DBLEND_SRCALPHA||
               FAILED(device->GetRenderState(D3DRS_DESTBLEND,&dst))||dst!=D3DBLEND_ONE||
               FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE,&on))||!on||
               FAILED(device->GetRenderState(D3DRS_BLENDOP,&op))||op!=D3DBLENDOP_ADD||
               FAILED(device->GetRenderState(D3DRS_ZWRITEENABLE,&zwrite))||zwrite)return false;
            // Attribution only: no device mutation and no substitute draw. The
            // exact source, shader and blend guards above also apply to omission.
            if(deathDecayOmit) {
                if(fadeReports++<8)WLOG_INFO("m2-dnd-mesh-attribution: omitted=1 owner=%p batch=%u; temporary diagnostic only",context.owner,context.index);
                result=D3D_OK;
                return true;
            }
            if(
               FAILED(shaderobjects::Pixel(device,reinterpret_cast<const DWORD*>(deathDecayDetail ? kDeathDecayDetailPS : deathDecayMask ? kDeathDecayMaskPS : deathDecayColour ? kDeathDecayColourShader : kDeathDecayFadeShader),&replacement.value))||
               FAILED(device->CreateStateBlock(D3DSBT_ALL,&state.value))||FAILED(state.value->Capture()))return false;
            if(deathDecayMask) {
                maskTexture.value=ResolveMask(context);
                bool ok=maskTexture.value && SUCCEEDED(shaderobjects::Vertex(device,reinterpret_cast<const DWORD*>(kDeathDecayMaskVS),&maskVS.value));
                if(ok)ok=SUCCEEDED(device->SetVertexShader(maskVS.value))&&SUCCEEDED(device->SetTexture(2,maskTexture.value))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_LINEAR))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_MAXMIPLEVEL,0))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_MIPMAPLODBIAS,0))&&
                    SUCCEEDED(device->SetSamplerState(2,D3DSAMP_SRGBTEXTURE,FALSE));
                if(!ok){if(FAILED(state.value->Apply())){deathDecayMask=false;WLOG_WARN("m2-dnd-mask: preparation restore failed; disabled");}return false;}
            }
            if(FAILED(device->SetPixelShader(replacement.value))) {
                if(FAILED(state.value->Apply())){deathDecayFade=false;deathDecayColour=false;deathDecayMask=false;WLOG_WARN("m2-dnd-fade: setup restore failed; disabled");}
                return false;
            }
            result=original(device,type,base,min,vertices,start,primitives);
            const bool restored=SUCCEEDED(state.value->Apply());
            if(fadeReports++<24)WLOG_INFO("%s: applied=1 owner=%p batch=%u restored=%u hr=%#lx; native blend retained",maskedMeshMaterials ? "m2-masked-mesh" : deathDecayMask ? "m2-dnd-mask" : deathDecayColour ? "m2-dnd-colour" : "m2-dnd-fade",context.owner,context.index,unsigned(restored),result);
            if(!restored){deathDecayFade=false;deathDecayColour=false;deathDecayMask=false;WLOG_WARN("m2-dnd-fade: restore failed; disabled");}
            return true;
        }
    }
    void Initialize() noexcept
    {
        try
        {
            enabled = materialconfig::Feature("WXL_M2_BLEND7_EXPERIMENT");
            deathDecayFade=wxl_modern_m2::ConfigBool("WXL_M2_DND_FADE",false);fadeReports=0;
            deathDecayOmit=wxl_modern_m2::ConfigBool("WXL_M2_DND_MESH_OMIT",false);
            deathDecayColour=wxl_modern_m2::ConfigBool("WXL_M2_DND_COLOUR",false);
            deathDecayMask=wxl_modern_m2::ConfigBool("WXL_M2_DND_MASK",false);
            maskedMeshMaterials=wxl_modern_m2::ConfigBool("WXL_M2_MASKED_MESH_MATERIALS",false);
            if(maskedMeshMaterials){deathDecayMask=true;deathDecayOmit=false;deathDecayFade=false;deathDecayColour=false;WLOG_WARN("m2-masked-mesh: metadata-family trial enabled; exact GPU contract still required");}
            deathDecayDetail=wxl_modern_m2::ConfigBool("WXL_M2_DND_DETAIL",false);
            if(deathDecayDetail){maskedMeshMaterials=false;deathDecayMask=true;deathDecayOmit=false;deathDecayFade=false;deathDecayColour=false;WLOG_WARN("m2-dnd-detail: RGB artwork trial enabled; exact DnD source guards retained");}
            if(deathDecayMask)WLOG_WARN("m2-dnd-mask: captured circular third-map approximation enabled; not full TXAC support");
            if(deathDecayColour)WLOG_WARN("m2-dnd-colour: exact Mod_Add mesh colour trial enabled; native alpha and blend retained");
            if(deathDecayOmit)WLOG_WARN("m2-dnd-mesh-attribution: exact two-mesh omission enabled; diagnostic only");
            if(deathDecayFade)WLOG_WARN("m2-dnd-fade: exact Mod_Add mesh alpha trial enabled; not full donor blending support");
            reports = 0;
            if (enabled) WLOG_WARN("m2-blend7-experiment: enabled; framebuffer-only A/B, not full material support; native shaders retained");
        }
        catch (...) { enabled = false; deathDecayFade = false; deathDecayOmit = false; deathDecayColour = false; deathDecayMask = false; maskedMeshMaterials = false; }
    }
    long Draw(void* rawDevice, void* rawContext, void* instance, int type, int baseVertex,
              unsigned minVertex, unsigned vertices, unsigned start, unsigned primitives,
              DrawFn original)
    {
        Context context{};
        if(rawDevice&&SelectFade(rawContext,instance,context)) {
            long result=0;
            if(DrawFade(static_cast<IDirect3DDevice9*>(rawDevice),context,type,baseVertex,minVertex,vertices,start,primitives,original,result))return result;
            if(fadeReports++<8)WLOG_INFO("%s: native fallback; shader/state contract or preparation failed",maskedMeshMaterials ? "m2-masked-mesh" : "m2-dnd-fade");
        }
        if (!enabled || !rawDevice || !Select(rawContext, instance, context))
            return original(rawDevice, type, baseVertex, minVertex, vertices, start, primitives);
        auto* device = static_cast<IDirect3DDevice9*>(rawDevice);
        if (!HasProgrammableTextures(device))
            return original(rawDevice, type, baseVertex, minVertex, vertices, start, primitives);
        ScopedBlend<IDirect3DDevice9> states(device);
        const bool applied = states.Apply();
        if (reports < 24)
        {
            ++reports;
            WLOG_INFO("m2-blend7-experiment: sample=%u owner=%p batch=%u applied=%u sourceBlend=7 liveBlend=4"
                " shader=0x4014 src=ONE dst=INVSRCALPHA; pre-DIP probe records original state",
                reports, context.owner, context.index, unsigned(applied));
        }
        const long result = original(rawDevice, type, baseVertex, minVertex, vertices, start, primitives);
        if (!states.Restore())
        {
            enabled = false;
            WLOG_WARN("m2-blend7-experiment: state restore failed; disabled for this process; restart client before more comparisons");
        }
        return result;
    }
}
