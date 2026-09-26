// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "RibbonShader.hpp"
#include "ShaderObjects.hpp"
#include "RibbonPolicy.hpp"
#include "ShaderDiagnosticIdentity.hpp"
#include "shaders/ParticlePrograms.hpp"
#include "shaders/RibbonPrograms.hpp"
#include "../ExtensionApi.hpp"
#include "../compat/MaterialConfig.hpp"
#include "../compat/ModernM2.hpp"
#include "game/M2.hpp"
#include "offsets/engine/Gx.hpp"
#include <d3d9.h>

namespace wxl::modern::ribbon {
namespace {
namespace off=wxl::offsets::game::m2;
namespace fmt=wxl::structure::m2;
namespace mat=assets::m2::material;
using BatchFn=void(__fastcall*)(void*,void*);
BatchFn originalBatch=nullptr;
unsigned mode=0,reports=0;
bool disabled=false;
template<class T>T Read(const void* p,size_t at){T v;std::memcpy(&v,static_cast<const uint8_t*>(p)+at,sizeof(v));return v;}
struct Identity {
    void* instance=nullptr;void* shared=nullptr;void* emitter=nullptr;
    const fmt::M2Header* header=nullptr;unsigned index=0;char path[276]{};
};
struct Frame {Identity id;std::shared_ptr<const mat::ModelSource> source;Recipe recipe;LayerRecipe layers;unsigned draws=0,applied=0;};
Frame* current=nullptr;
void* activeEmitter=nullptr;
struct Scope {Frame* previous=current;Scope(Frame* f){current=f;}~Scope(){current=previous;}};
bool Identify(void* raw,Identity& id) noexcept {
    if(!raw)return false;
    __try {
        auto* dc=static_cast<off::DrawContext*>(raw);
        if(!dc->instance||!dc->element||Read<unsigned>(dc->element,0)!=3||Read<void*>(dc->element,4)!=dc->instance)return false;
        id.instance=dc->instance;id.shared=reinterpret_cast<void*>(static_cast<off::M2Instance*>(dc->instance)->model);
        if(!id.shared)return false;
        wxl::game::m2::M2Model model(id.shared);id.header=model.GetHeader();id.index=Read<unsigned>(dc->element,0x18);
        if(!id.header||id.index>=id.header->ribbonEmitters.count||id.header->ribbonEmitters.count>128)return false;
        // CM2Model initializer stores CRibbonEmitter* in instance+0x2bc, indexed by source ribbon.
        auto** emitters=Read<void**>(id.instance,0x2bc);if(!emitters)return false;
        id.emitter=emitters[id.index];if(!id.emitter)return false;
        const char* path=model.GetPathStem();if(path) {
            unsigned n=0;for(;n+1<sizeof(id.path)&&path[n];++n)id.path[n]=path[n];
            if(path[n])return false;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReadTransform(const Frame& f,unsigned transform,float (&uv)[4]) noexcept {
    if(transform==0xffff){uv[0]=uv[1]=1;uv[2]=uv[3]=0;return true;}
    __try {
        if(transform>=f.id.header->textureTransforms.count)return false;
        const auto* matrices=Read<const float*>(f.id.instance,0xb0);if(!matrices)return false;
        return mode==3?NativeScaleOffset(matrices+16*transform,uv):Transform(matrices+16*transform,uv);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Same shared+0x174 fixed-texture array used by the native ribbon initializer.
// Caller captures D3D state BEFORE readiness/upload. No engine sampler cache writes.
IDirect3DBaseTexture9* Resolve(const Frame& f,unsigned layer) noexcept {
    __try {
        const auto index=f.layers.textures[layer];
        if(index>=f.id.header->textures.count)return nullptr;
        auto** handles=Read<void**>(f.id.shared,0x174);if(!handles||!handles[index])return nullptr;
        void* wrapper=reinterpret_cast<off::M2_TexResolveFn>(off::kTexResolve)(handles[index],0,0);
        void* gx=*reinterpret_cast<void**>(wxl::offsets::engine::gx::kGxDevicePtr);
        if(!wrapper||!gx)return nullptr;
        using ReadyFn=void(__fastcall*)(void*,void*,void*);
        reinterpret_cast<ReadyFn>((*reinterpret_cast<void***>(gx))[0])(gx,nullptr,wrapper);
        auto* texture=Read<IDirect3DBaseTexture9*>(wrapper,0x38);
        if(texture)texture->AddRef();return texture;
    } __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
bool NativePassReady(const Frame& f) noexcept {
    __try {return Read<unsigned>(f.id.emitter,0x118)==1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void __fastcall Batch(void* raw,void* edx) {
    Frame f;
    bool selected=false;
    if(mode && Identify(raw,f.id) && assets::m2::IsNativeLoaded(f.id.shared)) {
        f.source=mat::SourceMaterials().FindModel(f.id.header);
        selected=f.source && Decode(*f.source,f.id.index,f.recipe) && NativePassReady(f);
        if(selected&&mode==3)selected=DecodeLayers(*f.source,f.id.index,f.layers);
    }
    Scope scope(selected?&f:nullptr);
    originalBatch(raw,edx);
    if(selected && reports<48) {
        ++reports;
        WLOG_INFO("m2-ribbon-shader: batch model=%s instance=%p owner=%p ribbon=%u emitter=%p mode=%u draws=%u applied=%u sourceTextures=%u sourceMaterials=%u",f.id.path,f.id.instance,f.id.header,f.id.index,f.id.emitter,mode,f.draws,f.applied,f.source->ribbons[f.id.index].textureCount,f.source->ribbons[f.id.index].materialCount);
    }
}
template<class T>struct Com {T* p=nullptr;~Com(){if(p)p->Release();}};
template<class T,size_t N>bool Exact(T* s,const uint32_t (&known)[N]) {
    return materialdiag::InspectShader(s,reinterpret_cast<const uint8_t*>(known),sizeof(known)).exactKnown;
}
template<class T>void CaptureRejected(T* shader,const char* stage) {
    // Up to four unique programs per stage, captured at the actual rejection site.
    // Hash is grouping only; it never relaxes the exact ABI guard.
    static std::array<uint32_t,4> hashes{};static unsigned count=0;
    const auto id=materialdiag::InspectShader(shader,nullptr,0);
    if(id.state!=materialdiag::ShaderRead::Read||count>=hashes.size())return;
    for(unsigned n=0;n<count;++n)if(hashes[n]==id.hash)return;
    std::array<uint8_t,16384> bytes{};unsigned size=id.bytes;
    if(FAILED(shader->GetFunction(bytes.data(),&size))||size!=id.bytes)return;
    hashes[count++]=id.hash;
    WLOG_INFO("m2-ribbon-shader-rejected: stage=%s id=%u bytes=%u hash=%08x",stage,count,size,id.hash);
    for(unsigned at=0;at<size;at+=48){
        char hex[97]{};static constexpr char digits[]="0123456789abcdef";
        for(unsigned n=0;n<48&&at+n<size;++n){const auto b=bytes[at+n];hex[n*2]=digits[b>>4];hex[n*2+1]=digits[b&15];}
        WLOG_INFO("m2-ribbon-shader-rejected: stage=%s id=%u offset=%u hex=%s",stage,count,at,hex);
    }
}
struct Saved {
    Com<IDirect3DStateBlock9> block;bool captured=false;
    bool Restore(){if(!captured)return true;captured=false;return SUCCEEDED(block.p->Apply());}
    ~Saved(){Restore();}
};
}
bool Initialize() noexcept {
    mode=materialconfig::RibbonMode();
    if(!mode)return true;
    if(!wxl_modern_m2::HookAttachByName("M2.DrawRibbonBatch",&Batch,&originalBatch)){mode=0;return false;}
    WLOG_WARN("m2-ribbon-shader: build=ribbon-native-xy-20260906 mode=%u (1=marker, 2=reference UV, 3=three-map native XY candidate); donor ribbon formula unconfirmed; default off",mode);
    return true;
}
void PrepareSourcePasses(uint8_t* base,uint32_t size) noexcept {
    if(!mode||!base||size<sizeof(fmt::M2Header))return;
    const auto source=mat::SourceMaterials().FindModel(base);if(!source)return;
    fmt::M2Header h;std::memcpy(&h,base,sizeof(h));
    if(!h.ribbonEmitters.offset||h.ribbonEmitters.offset>size||h.ribbonEmitters.count>(size-h.ribbonEmitters.offset)/sizeof(fmt::M2Ribbon))return;
    for(unsigned n=0;n<h.ribbonEmitters.count;++n) {
        Recipe r;if(!Decode(*source,n,r))continue;
        LayerRecipe layers;if(mode==3&&!DecodeLayers(*source,n,layers))continue;
        auto* raw=base+h.ribbonEmitters.offset+n*sizeof(fmt::M2Ribbon);
        fmt::M2Ribbon before;std::memcpy(&before,raw,sizeof(before));
        if(before.materialIndices.count!=1||before.textureIndices.count!=source->ribbons[n].textureCount)continue;
        // The native initializer loops textureCount and reads materialIndices[i] without checking
        // materialCount. The reference renderer uses materialCount passes. Preserve all references
        // in the source sidecar, expose only that verified first pass to the stock initializer.
        const uint32_t one=1;std::memcpy(raw+offsetof(fmt::M2Ribbon,textureIndices),&one,sizeof(one));
        WLOG_INFO("m2-ribbon-shader: source owner=%p ribbon=%u sourceTextures=%u nativePasses=1 transform=%u",base,n,before.textureIndices.count,r.transform);
    }
}
bool Owns(void* emitter) noexcept {return current&&current->id.emitter==emitter;}
bool Active() noexcept {return current&&current->id.emitter==activeEmitter;}
void* SetEmitter(void* emitter) noexcept {void* previous=activeEmitter;activeEmitter=emitter;return previous;}
long Draw(void* device,int type,int base,unsigned min,unsigned vertices,unsigned start,unsigned primitives,DIPFn original) {
    Frame* f=current;if(!Active())return original(device,type,base,min,vertices,start,primitives);
    ++f->draws;
    auto* d=static_cast<IDirect3DDevice9*>(device);
    auto fallback=[&](const char* reason) {
        if(reports<48)WLOG_WARN("m2-ribbon-shader: fallback owner=%p ribbon=%u reason=%s",f->id.header,f->id.index,reason);
        return original(device,type,base,min,vertices,start,primitives);
    };
    if(disabled||!d||type!=D3DPT_TRIANGLESTRIP||!vertices||!primitives)return fallback("device-or-draw-shape");
    Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
    Com<IDirect3DVertexBuffer9> vb;Com<IDirect3DBaseTexture9> texture;
    UINT offset=0,stride=0;float uv[3][4]{{1,1,0,0},{1,1,0,0},{1,1,0,0}};
    if(FAILED(d->GetVertexShader(&vs.p))||!vs.p||FAILED(d->GetPixelShader(&ps.p))||!ps.p||
        FAILED(d->GetStreamSource(0,&vb.p,&offset,&stride))||!vb.p||stride!=24)return fallback("native-stream-or-shaders");
    // Marker has no interpolator inputs. SourceUV requires exact native shader/UV ABI.
    if(mode>=2 && (!Exact(vs.p,particlelayers::programs::NativeUnlit)||!Exact(ps.p,particlelayers::programs::NativePixel))){CaptureRejected(vs.p,"VS");CaptureRejected(ps.p,"PS");return fallback("native-shader-identity");}
    if(mode>=2 && (!ReadTransform(*f,f->recipe.transform,uv[0])||FAILED(d->GetTexture(0,&texture.p))||!texture.p||texture.p->GetType()!=D3DRTYPE_TEXTURE))return fallback("transform-or-texture");
    if(mode==3)for(unsigned n=0;n<3;++n)if(!ReadTransform(*f,f->layers.transforms[n],uv[n]))return fallback("layer-transform");
    Com<IDirect3DPixelShader9> shader;Saved saved;
    std::array<Com<IDirect3DBaseTexture9>,3> textures;
    const uint32_t* code=mode==1?programs::Marker:(mode==2?programs::SourceUV:programs::ThreeMapCandidate);
    if(FAILED(shaderobjects::Pixel(d,reinterpret_cast<const DWORD*>(code),&shader.p)))return fallback("create-ps");
    if(FAILED(d->CreateStateBlock(D3DSBT_ALL,&saved.block.p))||!saved.block.p||FAILED(saved.block.p->Capture()))return fallback("capture-state");
    saved.captured=true;
    bool ready=true;const char* reason="set-shader-state";
    if(mode==3) {
        reason="layer-texture-residency";
        for(unsigned n=0;n<3&&ready;++n){textures[n].p=Resolve(*f,n);ready=textures[n].p&&textures[n].p->GetType()==D3DRTYPE_TEXTURE;}
        if(ready){reason="texture0-identity";ready=textures[0].p==texture.p;}
        // Readiness may touch D3D bindings. Reapply the captured native baseline before
        // inheriting sampler policy and submitting geometry, while retaining it for final restore.
        if(ready&&FAILED(saved.block.p->Apply())){disabled=true;WLOG_ERROR("m2-ribbon-shader: STATE RESTORE FAILED after texture readiness; disabled; restart client");return D3DERR_INVALIDCALL;}
        if(ready){
            reason="layer-sampler-state";
            // Source wrap flags are independent per map; keep native filtering/LOD/sRGB policy.
            const D3DSAMPLERSTATETYPE inherited[]{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_MIPMAPLODBIAS,D3DSAMP_MAXMIPLEVEL,D3DSAMP_MAXANISOTROPY,D3DSAMP_SRGBTEXTURE};
            DWORD values[7]{};
            for(unsigned k=0;k<7&&ready;++k)ready=SUCCEEDED(d->GetSamplerState(0,inherited[k],&values[k]));
            for(unsigned n=0;n<3&&ready;++n){
                ready=SUCCEEDED(d->SetTexture(n,textures[n].p))&&
                    SUCCEEDED(d->SetSamplerState(n,D3DSAMP_ADDRESSU,(f->layers.flags[n]&1)?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP))&&
                    SUCCEEDED(d->SetSamplerState(n,D3DSAMP_ADDRESSV,(f->layers.flags[n]&2)?D3DTADDRESS_WRAP:D3DTADDRESS_CLAMP));
                for(unsigned k=0;k<7&&ready;++k)ready=SUCCEEDED(d->SetSamplerState(n,inherited[k],values[k]));
            }
        }
    }
    if(ready){reason="set-shader-state";ready=SUCCEEDED(d->SetPixelShader(shader.p)) && (mode==1||SUCCEEDED(d->SetPixelShaderConstantF(0,&uv[0][0],mode==3?3:1)));}
    long result=S_OK;
    if(ready)result=original(device,type,base,min,vertices,start,primitives);
    const bool restored=saved.Restore();
    if(!restored){disabled=true;WLOG_ERROR("m2-ribbon-shader: STATE RESTORE FAILED; disabled; restart client");return ready?result:D3DERR_INVALIDCALL;}
    if(!ready)return fallback(reason);
    ++f->applied;
    if(reports<48) {
        if(mode==3)for(unsigned n=0;n<3;++n)WLOG_INFO("m2-ribbon-shader: layer owner=%p ribbon=%u slot=%u texture=%u transform=%u flags=%u gpu=%p uv=(%.6g,%.6g,%.6g,%.6g)",f->id.header,f->id.index,n,f->layers.textures[n],f->layers.transforms[n],f->layers.flags[n],textures[n].p,uv[n][0],uv[n][1],uv[n][2],uv[n][3]);
        WLOG_INFO("m2-ribbon-shader: applied owner=%p ribbon=%u mode=%u uv=(%.6g,%.6g,%.6g,%.6g) vertices=%u primitives=%u hr=%#lx restored=1",f->id.header,f->id.index,mode,uv[0][0],uv[0][1],uv[0][2],uv[0][3],vertices,primitives,result);
    }
    return result;
}
}
