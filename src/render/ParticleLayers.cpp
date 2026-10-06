// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "ParticleLayers.hpp"
#include "ShaderObjects.hpp"
#include "ParticleLayerPolicy.hpp"
#include "RiftDiagnosticPolicy.hpp"
#include "ParticleIndexPolicy.hpp"
#include "ParticleMaterialState.hpp"
#include "ParticleNativeShaderPolicy.hpp"
#include "ParticleViewPolicy.hpp"
#include "RefractionAttributionPolicy.hpp"
#include "ParticleRefraction.hpp"
#include "ParticleDiagnosticPolicy.hpp"
#include "TexturePayloadAudit.hpp"
#include "MaterialDiagnosticPolicy.hpp"
#include "shaders/ParticlePrograms.hpp"
#include "../ExtensionApi.hpp"
#include "../compat/MaterialConfig.hpp"
#include "../compat/ModernM2.hpp"
#include "game/M2.hpp"
#include "offsets/engine/Gx.hpp"
#include <d3d9.h>
#include <vector>

namespace wxl::modern::particlelayers {
namespace {
    namespace off=wxl::offsets::game::m2;
    namespace mat=assets::m2::material;
    namespace fmt=wxl::structure::m2;
    bool enabled=false;
    bool materialEnabled=false;
    bool modulationEnabled=false;
    bool textureAuditEnabled=false;
    bool riftNativeSpritesEnabled=false;
    bool omitBloodRefraction=false;
    bool bloodRefractionEnabled=false;
    bool bloodSmokeEnabled=false;
    bool bloodPlagueEnabled=false;
    bool spriteMaterialsEnabled=false;
    char textureAuditPath[276]{};
    materialdiag::ProbeBudget textureAuditBudget{48,2,500};
    bool viewEnabled=false;
    bool hideAllEnabled=false;
    bool traceEnabled=false;
    void* traceInstance=nullptr;
    unsigned viewEmitter=UINT_MAX,viewMode=0;
    char viewPath[276]{};
    using BuildFn=uint32_t(__fastcall*)(void*,void*,void*,void*);
    BuildFn originalBuild=nullptr;
    template<class T>T Read(const void* ptr,size_t offset) {T value;std::memcpy(&value,static_cast<const uint8_t*>(ptr)+offset,sizeof(value));return value;}
    struct Identity {void* shared=nullptr;const fmt::M2Header* owner=nullptr;void* emitter=nullptr;unsigned index=0;};
    struct Frame {
        Identity id;
        std::shared_ptr<const mat::ModelSource> source;
        Recipe recipe{};
        std::vector<uint8_t> vertices;
        unsigned nativeStride=0,vertexCount=0;
        unsigned nativeKind=0;
        float minAge=3600,maxAge=0;
        bool valid=true;
        bool nativeSprite=false;
        bool viewTarget=false;
        bool diagnosticOmit=false;
        bool bloodRefraction=false;
        bool bloodSmoke=false;
        bool bloodPlague=false;
        bool spriteMaterial=false;
    };
    struct ReportKey {const void* owner=nullptr;unsigned emitter=0,stage=0,count=0;const char* reason=nullptr;};
    std::array<ReportKey,32> appliedReports{},skipReports{};
    std::array<ReportKey,32> inputReports{};
    std::array<ReportKey,32> viewReports{};
    std::array<ReportKey,32> admissionReports{};
    particlediag::ShaderSet inputShaders;
    unsigned inputId=0;
    bool Report(std::array<ReportKey,32>& keys,const Frame& frame,const char* reason,unsigned stage) noexcept {
        for(auto& key:keys) {
            if(!key.count){key={frame.id.owner,frame.id.index,stage,1,reason};return true;}
            if(key.owner==frame.id.owner&&key.emitter==frame.id.index&&key.stage==stage&&key.reason==reason)
                {if(key.count>=2)return false;++key.count;return true;}
        }
        return false;
    }
    bool nativeSpriteEnabled=false;
    Frame* current=nullptr;
    bool RiftTarget(void* raw) noexcept {
        if(!riftdiag::enabled.load() || !raw)return false;
        __try {
            auto* dc=static_cast<off::DrawContext*>(raw);
            if(!dc->instance)return false;
            auto* shared=reinterpret_cast<void*>(static_cast<off::M2Instance*>(dc->instance)->model);
            if(!shared)return false;
            const char* source=wxl::game::m2::M2Model(shared).GetPathStem();
            if(!source)return false;
            char path[276]{};size_t n=0;
            for(;n+1<sizeof(path)&&source[n];++n)path[n]=source[n];
            return !source[n]&&riftdiag::Target(path);
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    struct Coverage {
        unsigned build=0,dispatch=0,oneShot=0,ribbon=0,inactive=0,adapter=0,hidden=0,submitted=0,fallback=0,failed=0;
    };
    Coverage* coverage=nullptr;
    // Broader than eligibility, deliberately: matching paths rejected before IsViewTarget
    // must still have an audit row. This function is read-only and never authorizes hiding.
    bool AuditTarget(void* raw) noexcept {
        if(!viewEnabled||!raw)return false;
        __try {
            const auto* dc=static_cast<off::DrawContext*>(raw);
            if(!dc->instance)return false;
            auto* shared=reinterpret_cast<void*>(static_cast<off::M2Instance*>(dc->instance)->model);
            if(!shared)return false;
            const char* source=wxl::game::m2::M2Model(shared).GetPathStem();
            if(!source)return false;
            char path[276]{};size_t n=0;
            for(;n+1<sizeof(path)&&source[n];++n)path[n]=source[n];
            return !source[n]&&ViewPathMatches(path,viewPath);
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void* ReadInstance(void* raw) noexcept {
        __try {return raw?static_cast<off::DrawContext*>(raw)->instance:nullptr;}
        __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    bool IsViewTarget(const Frame& frame) noexcept {
        if(!viewEnabled)return false;
        __try {
            char path[276]{};const char* source=wxl::game::m2::M2Model(frame.id.shared).GetPathStem();
            if(!source)return false;
            size_t n=0;for(;n+1<sizeof(path)&&source[n];++n)path[n]=source[n];
            if(source[n])return false;
            if(!ViewTarget(true,path,viewPath,frame.id.owner->vertices.count,unsigned(frame.source->particles.size()),viewEmitter))return false;
            Recipe recipe{};
            for(unsigned i=0;i<frame.source->particles.size();++i)if(!Decode(*frame.source,i,recipe))return false;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool Identify(void* raw,Identity& out) noexcept {
        if(!raw)return false;
        __try {
            auto* dc=static_cast<off::DrawContext*>(raw);
            if(!dc->instance||!dc->element||Read<uint32_t>(dc->element,0)!=4||Read<void*>(dc->element,4)!=dc->instance)return false;
            out.emitter=Read<void*>(dc->element,0x18);
            out.shared=reinterpret_cast<void*>(static_cast<off::M2Instance*>(dc->instance)->model);
            if(!out.shared||!out.emitter)return false;
            out.owner=wxl::game::m2::M2Model(out.shared).GetHeader();
            if(!out.owner || Read<uint32_t>(out.emitter,0x8c)!=4 || Read<uint32_t>(out.emitter,0x90)!=6)return false;
            return particlediag::EmitterIndex(out.owner->particleEmitters.offset,out.owner->particleEmitters.count,
                Read<uintptr_t>(out.emitter,off::kOffEmitterHeadCellBlock),out.index);
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool RefractionAttributionTarget(const Frame& frame) noexcept {
        if((!omitBloodRefraction && !bloodRefractionEnabled) || !frame.source)return false;
        __try {
            const char* source=wxl::game::m2::M2Model(frame.id.shared).GetPathStem();
            if(!source)return false;
            char path[276]{};size_t n=0;
            for(;n+1<sizeof(path)&&source[n];++n)path[n]=source[n];
            return !source[n]&&MatchBloodBoilRefraction(true,path,*frame.source,frame.id.index);
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool ExactParticlePath(const Frame& frame,const char* wanted) noexcept {
        __try {
            const char* source=wxl::game::m2::M2Model(frame.id.shared).GetPathStem();
            if(!source)return false;
            char path[276]{};size_t n=0;
            for(;n+1<sizeof(path)&&source[n];++n)path[n]=source[n];
            return !source[n]&&ViewPathMatches(path,wanted);
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    struct VertexSnapshot {uintptr_t position=0;unsigned stride=0,count=0;float age=0;};
    bool Snapshot(void* particle,void* writer,VertexSnapshot& out) noexcept {
        if(!particle||!writer)return false;
        __try {
            out.position=Read<uintptr_t>(writer,0);out.stride=Read<uint32_t>(writer,16);out.count=Read<uint32_t>(writer,32);
            out.age=Read<float>(particle,0);
            return out.position&&(out.stride==24||out.stride==36)&&out.count<=4096&&std::isfinite(out.age)&&out.age>=0&&out.age<3600;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool CopyQuad(void* writer,const VertexSnapshot& before,uint8_t (&bytes)[144],unsigned& added) noexcept {
        __try {
            const auto count=Read<uint32_t>(writer,32);
            if(count<before.count)return false;
            added=count-before.count;
            if(!added)return true;
            if(added!=4||Read<uint32_t>(writer,16)!=before.stride||
                Read<uintptr_t>(writer,0)!=before.position+4*before.stride)return false;
            std::memcpy(bytes,reinterpret_cast<void*>(before.position),4*before.stride);return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    uint32_t __fastcall Build(void* emitter,void* edx,void* particle,void* writer) {
        if(coverage)++coverage->build;
        Frame* frame=current;
        VertexSnapshot before;
        const bool selected=enabled&&frame&&!frame->diagnosticOmit&&!frame->bloodRefraction&&frame->valid&&frame->id.emitter==emitter;
        const bool captured=selected&&Snapshot(particle,writer,before);
        const uint32_t result=originalBuild(emitter,edx,particle,writer);
        if(!selected)return result;
        try {
            uint8_t native[144]{};unsigned added=0;
            if(!captured||!CopyQuad(writer,before,native,added)){frame->valid=false;return result;}
            if(!added)return result;
            if(frame->vertexCount!=before.count||frame->vertexCount+4>4096||
                (frame->nativeStride&&frame->nativeStride!=before.stride)){frame->valid=false;return result;}
            uint8_t expanded[208]{};
            const uint32_t seed=uint32_t(reinterpret_cast<uintptr_t>(particle))^uint32_t(reinterpret_cast<uintptr_t>(emitter));
            if(!ExpandQuad(native,before.stride,before.age,seed,frame->recipe,expanded)){frame->valid=false;return result;}
            frame->vertices.insert(frame->vertices.end(),expanded,expanded+4*(before.stride+16));
            frame->nativeStride=before.stride;frame->vertexCount+=4;
            frame->minAge=std::min(frame->minAge,before.age);frame->maxAge=std::max(frame->maxAge,before.age);
        }catch(...){frame->valid=false;}
        return result;
    }
    template<class T>struct Com {T* value=nullptr;~Com(){if(value)value->Release();}};
    template<class Shader,size_t N>bool Exact(Shader* shader,const uint32_t (&expected)[N]) noexcept {
        particlediag::ShaderBytes bytes;
        return particlediag::ReadShader(shader,bytes)&&bytes.size==sizeof(expected)&&!std::memcmp(bytes.data.data(),expected,sizeof(expected));
    }
    // Independent budget at the actual guard site; general pre-DIP probes can exhaust earlier.
    // Reads only owned vertex copies and current D3D objects, never native write-only VB contents.
    void CaptureInputs(IDirect3DDevice9* d,const Frame& frame,IDirect3DVertexShader9* vs,unsigned known,bool matched) {
        if(!Report(inputReports,frame,matched?"matched":"mismatch",known))return;
        const unsigned id=++inputId;
        particlediag::ShaderBytes bytes;bool added=false;
        const unsigned dump=particlediag::ReadShader(vs,bytes)?inputShaders.Insert(bytes,added):0;
        WLOG_INFO("m2-particle-layer-input: id=%u owner=%p emitter=%u matched=%u knownVS=%u stride=%u vertices=%u ageMin=%.5f ageMax=%.5f material=%u sourceFlags=%#x",id,frame.id.shared,frame.id.index,unsigned(matched),known,frame.nativeStride,frame.vertexCount,frame.minAge,frame.maxAge,unsigned(materialEnabled),frame.source->particles[frame.id.index].Flags());
        WLOG_INFO("m2-particle-layer-input: id=%u shader=VS dumpId=%u bytes=%u new=%u",id,dump,bytes.size,unsigned(added));
        if(added)for(unsigned at=0;at<bytes.size;at+=32) {
            char hex[65]{};const unsigned count=std::min(32u,bytes.size-at);
            for(unsigned n=0;n<count;++n)sprintf_s(hex+2*n,sizeof(hex)-2*n,"%02x",unsigned(bytes.data[at+n]));
            WLOG_INFO("m2-particle-layer-shader: dumpId=%u offset=%u hex=%s",dump,at,hex);
        }
        Com<IDirect3DVertexDeclaration9> declaration;
        if(SUCCEEDED(d->GetVertexDeclaration(&declaration.value))&&declaration.value) {
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=MAXD3DDECLLENGTH+1;
            if(SUCCEEDED(declaration.value->GetDeclaration(elements,&count))&&count<=MAXD3DDECLLENGTH+1)
                for(unsigned n=0;n<count;++n) {const auto& e=elements[n];
                    WLOG_INFO("m2-particle-layer-input: id=%u decl=%u stream=%u offset=%u type=%u method=%u usage=%u index=%u",id,n,unsigned(e.Stream),unsigned(e.Offset),unsigned(e.Type),unsigned(e.Method),unsigned(e.Usage),unsigned(e.UsageIndex));}
        }
        std::array<unsigned,4> lo{255,255,255,255},hi{};unsigned first=0,last=0;
        for(unsigned n=0;n<frame.vertexCount;++n) {
            const unsigned packed=Read<unsigned>(frame.vertices.data()+size_t(n)*(frame.nativeStride+16),frame.nativeStride-12);
            if(!n)first=packed;last=packed;
            for(unsigned channel=0;channel<4;++channel){const auto c=(packed>>(8*channel))&255;lo[channel]=std::min(lo[channel],c);hi[channel]=std::max(hi[channel],c);}
        }
        WLOG_INFO("m2-particle-layer-input: id=%u packedARGBFirst=%08x packedARGBLast=%08x minRGBA=(%u,%u,%u,%u) maxRGBA=(%u,%u,%u,%u)",id,first,last,lo[2],lo[1],lo[0],lo[3],hi[2],hi[1],hi[0],hi[3]);
        for(unsigned reg : {10u,11u,12u,17u,18u,21u,22u,25u,26u,27u,29u,30u}) {
            float values[4]{};
            if(SUCCEEDED(d->GetVertexShaderConstantF(reg,values,1)))WLOG_INFO("m2-particle-layer-input: id=%u vsC=%u value=(%.6g,%.6g,%.6g,%.6g)",id,reg,values[0],values[1],values[2],values[3]);
        }
    }
    // Engine-owned model textures. Mode 0 is nonblocking; no path loading or CTexture allocation.
    // Gx's virtual slot 0 performs the same readiness/upload step used by ISetTexture.
    IDirect3DBaseTexture9* Resolve(Frame& frame,unsigned layer,const char*& reason) noexcept {
        __try {
            const auto index=frame.recipe.indices[layer];
            reason="texture-index";
            if(index>=frame.id.owner->textures.count)return nullptr;
            auto** handles=Read<void**>(frame.id.shared,0x174);
            reason="texture-handle";
            if(!handles||!handles[index])return nullptr;
            auto resolve=reinterpret_cast<off::M2_TexResolveFn>(off::kTexResolve);
            void* wrapper=resolve(handles[index],0,0);
            void* gx=*reinterpret_cast<void**>(wxl::offsets::engine::gx::kGxDevicePtr);
            reason="texture-wrapper-or-device";
            if(!wrapper||!gx)return nullptr;
            using ReadyFn=void(__fastcall*)(void*,void*,void*);
            reinterpret_cast<ReadyFn>((*reinterpret_cast<void***>(gx))[0])(gx,nullptr,wrapper);
            reason="texture-residency";
            auto* texture=Read<IDirect3DBaseTexture9*>(wrapper,0x38);
            if(texture)texture->AddRef();
            return texture;
        } __except(EXCEPTION_EXECUTE_HANDLER){reason="texture-native-exception";return nullptr;}
    }
    struct Prepared {
        Com<IDirect3DStateBlock9> saved;
        Com<IDirect3DVertexBuffer9> vertices;
        Com<IDirect3DIndexBuffer9> indices;
        Com<IDirect3DVertexDeclaration9> declaration;
        Com<IDirect3DVertexShader9> vertex;
        Com<IDirect3DPixelShader9> pixel;
        std::array<Com<IDirect3DBaseTexture9>,3> textures;
        bool captured=false;
        const char* reason="draw-shape";
        HRESULT hr=S_OK;
        unsigned textureStage=UINT_MAX;
        bool Check(HRESULT value,const char* operation) noexcept {hr=value;reason=operation;return SUCCEEDED(value);}
        bool Gate(bool value,const char* operation) noexcept {reason=operation;hr=value?S_OK:S_FALSE;return value;}
        bool Restore() noexcept {if(!captured)return true;captured=false;return SUCCEEDED(saved.value->Apply());}
        ~Prepared(){Restore();}
    };
    bool TextureAuditTarget(const Frame& frame) noexcept {
        if(!textureAuditEnabled||!textureAuditPath[0])return false;
        __try {
            const char* source=wxl::game::m2::M2Model(frame.id.shared).GetPathStem();
            if(!source)return false;
            char path[276]{};size_t n=0;for(;n+1<sizeof(path)&&source[n];++n)path[n]=source[n];
            return !source[n]&&materialdiag::MatchesPath(path,textureAuditPath);
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool AuditTexture(const Frame& frame,unsigned stage,IDirect3DBaseTexture9* texture) {
        if(!TextureAuditTarget(frame))return true;
        const materialdiag::ProbeKey key{frame.id.owner,frame.id.shared,frame.source.get(),nullptr,frame.id.index*3+stage};
        if(!textureAuditBudget.Take(key,GetTickCount()))return true;
        const auto index=frame.recipe.indices[stage];
        const auto txid=index<frame.source->textureFileDataIds.size()?frame.source->textureFileDataIds[index]:0u;
        const auto result=textureaudit::Read(static_cast<IDirect3DTexture9*>(texture));
        WLOG_INFO("m2-particle-texture-audit: owner=%p emitter=%u stage=%u texture=%u txid=%u gpu=%p width=%u height=%u format=%u pool=%u bytes=%u fnv64=%016llx complete=%u reason=%s hr=%#lx",
            frame.id.shared,frame.id.index,stage,index,txid,texture,result.desc.Width,result.desc.Height,unsigned(result.desc.Format),unsigned(result.desc.Pool),result.layout.bytes,
            static_cast<unsigned long long>(result.hash),unsigned(result.complete),result.reason,result.hr);
        if(result.unlockFailed){textureAuditEnabled=false;return false;}
        return true; // Unsupported/nonresident readback is diagnostic-only, not a render gate.
    }
    bool Prepare(IDirect3DDevice9* d,Frame& frame,Prepared& out,int base,unsigned min,unsigned start,unsigned primitives) {
        Com<IDirect3DVertexShader9> nativeVs;Com<IDirect3DPixelShader9> nativePs;
        Com<IDirect3DVertexBuffer9> nativeVb;UINT offset=0,stride=0;
        if(!out.Check(d->GetVertexShader(&nativeVs.value),"get-vs")||!out.Check(d->GetPixelShader(&nativePs.value),"get-ps")||
            !out.Check(d->GetStreamSource(0,&nativeVb.value,&offset,&stride),"get-stream")||
            !out.Gate(nativeVb.value&&stride==frame.nativeStride,"native-stride")||
            !out.Gate(Exact(nativePs.value,programs::NativePixel),"native-ps-mismatch"))return false;
        const uint32_t* vs=nullptr;
        particlediag::ShaderBytes nativeBytes;
        const auto kind=particlediag::ReadShader(nativeVs.value,nativeBytes)?IdentifyNativeVertex(nativeBytes.data.data(),nativeBytes.size):NativeVertexKind::Unknown;
        const unsigned known=unsigned(kind);frame.nativeKind=known;
        if(NativeStrideMatches(kind,stride))switch(kind) {
            case NativeVertexKind::Unlit:vs=programs::Unlit;break;
            case NativeVertexKind::Directional:vs=programs::Directional;break;
            case NativeVertexKind::DirectionalPoint:vs=programs::DirectionalPoint;break;
            case NativeVertexKind::DirectionalTwoPoints:vs=programs::DirectionalTwoPoints;break;
            default:break;
        }
        CaptureInputs(d,frame,nativeVs.value,known,vs!=nullptr);
        if(!out.Gate(vs!=nullptr,"native-vs-mismatch"))return false;
        // Keep strict native ABI identity guards; source multitexture selects unlit shading
        // only after the native transform/UV/fog contract is known. Both declarations carry COLOR0.
        if(materialEnabled)vs=programs::Unlit;
        Com<IDirect3DIndexBuffer9> nativeIb;D3DINDEXBUFFER_DESC ibDesc{};D3DVERTEXBUFFER_DESC vbDesc{};
        if(!out.Check(d->GetIndices(&nativeIb.value),"get-indices")||!out.Gate(nativeIb.value!=nullptr,"native-indices-missing")||
            !out.Check(nativeIb.value->GetDesc(&ibDesc),"index-desc")||!out.Check(nativeVb.value->GetDesc(&vbDesc),"vertex-desc")||
            !out.Gate(ibDesc.Format==D3DFMT_INDEX16,"index-format")||
            !out.Gate(QuadRanges(base,min,frame.vertexCount,start,primitives,offset,stride,vbDesc.Size,ibDesc.Size),"native-buffer-ranges"))return false;
        if(frame.nativeSprite && frame.source->particles[frame.id.index].Blend()==2) {
            DWORD on=0,src=0,dst=0,op=0;
            if(!out.Check(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&on),"alpha-state") ||
               !out.Check(d->GetRenderState(D3DRS_SRCBLEND,&src),"alpha-src") ||
               !out.Check(d->GetRenderState(D3DRS_DESTBLEND,&dst),"alpha-dst") ||
               !out.Check(d->GetRenderState(D3DRS_BLENDOP,&op),"alpha-op") ||
               !out.Gate(on && src==D3DBLEND_SRCALPHA && dst==D3DBLEND_INVSRCALPHA && op==D3DBLENDOP_ADD,"alpha-native-contract"))return false;
        }
        // Capture before readiness/upload or any other mutations. Resources are strictly draw-local.
        if(!out.Check(d->CreateStateBlock(D3DSBT_ALL,&out.saved.value),"create-state-block")||!out.Check(out.saved.value->Capture(),"capture-state"))return false;
        out.captured=true;
        // Compare against the actual pre-upload binding, not readiness's incidental binding.
        Com<IDirect3DBaseTexture9> bound;
        if(!out.Check(d->GetTexture(0,&bound.value),"get-texture0"))return false;
        for(unsigned n=0;n<3;++n){out.textureStage=n;out.textures[n].value=Resolve(frame,n,out.reason);
            if(!out.textures[n].value)return false;
            if(!out.Gate(out.textures[n].value->GetType()==D3DRTYPE_TEXTURE,"texture-type"))return false;}
        out.textureStage=UINT_MAX;
        if(!out.Gate(bound.value==out.textures[0].value,"texture0-identity"))return false;
        for(unsigned n=0;n<3;++n)if(!out.Gate(AuditTexture(frame,n,out.textures[n].value),"texture-audit-unlock"))return false;
        if(!out.Gate(textureaudit::RestoreAfterReadiness(out.saved.value),"post-readiness-state"))return false;
        const auto* ps=frame.recipe.combine==pm::Combine::ThreeColorThreeAlpha?programs::ThreeColorThreeAlpha:programs::TwoColorThreeAlpha;
        if(frame.viewTarget&&frame.id.index==viewEmitter&&viewMode)ps=programs::LayerView;
        if(!out.Check(shaderobjects::Vertex(d,reinterpret_cast<const DWORD*>(vs),&out.vertex.value),"create-vs")||
            !out.Check(shaderobjects::Pixel(d,reinterpret_cast<const DWORD*>(ps),&out.pixel.value),"create-ps"))return false;
        const WORD color=WORD(stride-12),uv=WORD(stride-8);
        D3DVERTEXELEMENT9 elements[7]{};unsigned n=0;
        elements[n++]={0,0,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_POSITION,0};
        if(stride==36)elements[n++]={0,12,D3DDECLTYPE_FLOAT3,0,D3DDECLUSAGE_NORMAL,0};
        elements[n++]={0,color,D3DDECLTYPE_D3DCOLOR,0,D3DDECLUSAGE_COLOR,0};
        elements[n++]={0,uv,D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,0};
        elements[n++]={0,WORD(stride),D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,1};
        elements[n++]={0,WORD(stride+8),D3DDECLTYPE_FLOAT2,0,D3DDECLUSAGE_TEXCOORD,2};
        const D3DVERTEXELEMENT9 end=D3DDECL_END();elements[n]=end;
        if(!out.Check(d->CreateVertexDeclaration(elements,&out.declaration.value),"create-declaration"))return false;
        if(!out.Check(d->CreateVertexBuffer(UINT(frame.vertices.size()),D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&out.vertices.value,nullptr),"create-vb"))return false;
        void* data=nullptr;
        if(!out.Check(out.vertices.value->Lock(0,UINT(frame.vertices.size()),&data,0),"lock-vb"))return false;
        std::memcpy(data,frame.vertices.data(),frame.vertices.size());
        if(!out.Check(out.vertices.value->Unlock(),"unlock-vb"))return false;
        std::vector<uint16_t> indexData;
        if(!out.Gate(QuadIndices(frame.vertexCount,indexData),"quad-indices"))return false;
        const UINT indexBytes=UINT(indexData.size()*sizeof(uint16_t));
        if(!out.Check(d->CreateIndexBuffer(indexBytes,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&out.indices.value,nullptr),"create-ib")||
            !out.Check(out.indices.value->Lock(0,indexBytes,&data,0),"lock-ib"))return false;
        std::memcpy(data,indexData.data(),indexBytes);
        if(!out.Check(out.indices.value->Unlock(),"unlock-ib")||!out.Check(d->SetIndices(out.indices.value),"bind-ib"))return false;
        if(!out.Check(d->SetStreamSource(0,out.vertices.value,0,stride+16),"bind-vb")||!out.Check(d->SetVertexDeclaration(out.declaration.value),"bind-declaration")||
            !out.Check(d->SetVertexShader(out.vertex.value),"bind-vs")||!out.Check(d->SetPixelShader(out.pixel.value),"bind-ps"))return false;
        // BlendAdd uses black fog; new alpha-blend sprites retain native blending and fog.
        // Source animated cutoff is verified zero; 1/255 is this blend family's material alpha test.
        const float modulation=pm::SourceRGBModulationCandidate(modulationEnabled,frame.source->particles[frame.id.index].Flags());
        const bool blendAdd=frame.source->particles[frame.id.index].Blend()==7;
        const float parameters[4]{frame.recipe.multipliers[0]*modulation,frame.recipe.multipliers[1],materialEnabled?1.0f/255.0f:0.0f,materialEnabled&&blendAdd?1.0f:0.0f};
        if(!out.Check(d->SetPixelShaderConstantF(0,parameters,1),"bind-constants"))return false;
        if(frame.viewTarget&&frame.id.index==viewEmitter&&viewMode) {
            const float diagnostic[4]{float(viewMode),0,0,0};
            if(!out.Check(d->SetPixelShaderConstantF(1,diagnostic,1),"bind-view-constants"))return false;
        }
        if(materialEnabled&&blendAdd&&!out.Check(ApplyBlendAdd(d,out.textureStage),"blendadd-state"))return false;
        for(unsigned stage=0;stage<3;++stage){
            out.textureStage=stage;
            if(!out.Check(d->SetTexture(stage,out.textures[stage].value),"bind-texture"))return false;
            for(const auto state : {D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})if(!out.Check(d->SetSamplerState(stage,state,D3DTADDRESS_WRAP),"sampler-wrap"))return false;
            for(const auto state : {D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})if(!out.Check(d->SetSamplerState(stage,state,D3DTEXF_LINEAR),"sampler-filter"))return false;
            if(!out.Check(d->SetSamplerState(stage,D3DSAMP_MIPFILTER,D3DTEXF_POINT),"sampler-mip")||!out.Check(d->SetSamplerState(stage,D3DSAMP_SRGBTEXTURE,0),"sampler-srgb"))return false;
        }
        return true;
    }
}
bool Initialize() noexcept {
    try {
        enabled=materialconfig::Feature("WXL_M2_PARTICLE_LAYERS");
        materialEnabled=enabled&&materialconfig::Feature("WXL_M2_PARTICLE_MATERIAL");
        modulationEnabled=materialEnabled&&materialconfig::Feature("WXL_M2_PARTICLE_MODULATION");
        if(materialconfig::Profile()==1)WLOG_INFO("m2-material-profile: profile=1 layers=%u material=%u modulation=%u meshBlend=%u meshLighting=%u ribbon=%u; guarded subset, per-feature overrides retained; diagnostics unchanged",
            unsigned(enabled),unsigned(materialEnabled),unsigned(modulationEnabled),unsigned(materialconfig::Feature("WXL_M2_BLEND7_EXPERIMENT")),
            unsigned(materialconfig::Feature("WXL_M2_BLEND7_SOURCE_LIGHTING")),materialconfig::RibbonMode());
        textureAuditPath[0]=0;
        textureAuditEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_TEXTURE_AUDIT",false);
        nativeSpriteEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_NATIVE_SPRITE_LAYERS",false);
        bloodSmokeEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_BLOOD_SMOKE_APPROX",false);
        bloodPlagueEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_BLOOD_PLAGUE_APPROX",false);
        spriteMaterialsEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_SPRITE_MATERIALS",false);
        if(spriteMaterialsEnabled)WLOG_WARN("m2-sprite-materials: metadata-selected blend2/7 layers enabled; neutral and TXAC1/1 only; UV approximation, not universal shader support");
        if(bloodPlagueEnabled)WLOG_WARN("m2-blood-plague: exact target emitter1 colour composition enabled; TXAC UV approximation, NOT retail parity");
        if(bloodSmokeEnabled)WLOG_WARN("m2-blood-smoke: exact emitters1/2/5/6 colour composition enabled; 2/5 neutral metadata, 1/6 TXAC UV approximation; NOT retail parity");
        omitBloodRefraction=enabled&&wxl_modern_m2::ConfigBool("WXL_M2_BLOOD_REFRACTION_OMIT",false);
        bloodRefractionEnabled=enabled&&!omitBloodRefraction&&wxl_modern_m2::ConfigBool("WXL_M2_BLOOD_REFRACTION",false);
        if(bloodRefractionEnabled)WLOG_WARN("m2-blood-refraction: enabled exact Blood Boil emitter0 height-gradient approximation; max4px; NOT retail parity");
        if(omitBloodRefraction)WLOG_WARN("m2-refraction-attribution: diagnostic enabled; only exact Blood Boil emitter0 distortion map omitted; NOT a visual fix");
        riftNativeSpritesEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_RIFT_NATIVE_SPRITES",false);
        if(textureAuditEnabled) {
            wxl_modern_m2::ConfigRaw("WXL_M2_PARTICLE_PROBE_PATH",textureAuditPath,sizeof(textureAuditPath));
            textureAuditPath[sizeof(textureAuditPath)-1]=0;
            textureAuditEnabled=textureAuditPath[0]!=0;
            if(textureAuditEnabled)WLOG_WARN("m2-particle-texture-audit: enabled bounded managed DXT mip0 read-only payload fingerprints; not GPU pixel parity");
        }
        if(modulationEnabled)WLOG_WARN("m2-particle-modulation: enabled RGB-only source-selected 2x/4x candidate; alpha/UV unchanged; not recovered donor parity");
        viewPath[0]=0;
        viewEnabled=materialEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_VIEW",false);
        hideAllEnabled=false;
        if(viewEnabled) {
            wxl_modern_m2::ConfigRaw("WXL_M2_PARTICLE_VIEW_PATH",viewPath,sizeof(viewPath));
            viewEmitter=wxl_modern_m2::ConfigU32("WXL_M2_PARTICLE_VIEW_EMITTER",UINT_MAX,0,UINT_MAX);
            viewMode=wxl_modern_m2::ConfigU32("WXL_M2_PARTICLE_VIEW_MODE",UINT_MAX,0,UINT_MAX);
            viewEnabled=viewPath[0]&&viewEmitter<2048&&ValidView(viewMode);
            if(viewEnabled)WLOG_WARN("m2-particle-view: enabled exactPath='%s' emitter=%u mode=%u; DIAGNOSTIC hides other eligible emitters, not a visual fix",viewPath,viewEmitter,viewMode);
            else WLOG_WARN("m2-particle-view: invalid selector; diagnostic disabled");
        }
        hideAllEnabled=viewEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_HIDE_ALL",false);
        traceEnabled=viewEnabled&&wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_DRAW_TRACE",false);
        if(viewEnabled)WLOG_WARN("m2-particle-coverage: enabled hideAll=%u; one row per exact-path batch, including early bypasses; no sample budget",unsigned(hideAllEnabled));
        if(enabled&&!originalBuild&&!wxl_modern_m2::HookAttachByName("M2.ParticleBuildVertex",&Build,&originalBuild))enabled=false;
        if(nativeSpriteEnabled)WLOG_INFO("m2-native-sprite-layers: neutral TXAC emitters enabled; native simulation retained; alpha blending preserved; nonzero TXAC/refraction remain native");
        if(enabled)WLOG_WARN("m2-particle-layers: experimental layer/UV adapter enabled; material=%u (1=unlit BlendAdd black-fog material-alpha-test); deterministic slot seeds, validated EXP2 subset; not donor parity",unsigned(materialEnabled));
    }catch(...){enabled=false;}
    return enabled;
}
uint32_t AroundDraw(particlediag::DrawFn original,void* context,void* edx,uint32_t first,void* elements,const uint32_t* order,uint32_t end) {
    if(!enabled)return original(context,edx,first,elements,order,end);
    Frame frame;
    Coverage audit;
    Coverage* previousCoverage=coverage;
    void* previousInstance=traceInstance;
    traceInstance=traceEnabled?ReadInstance(context):nullptr;
    const bool audited=AuditTarget(context);
    coverage=audited?&audit:nullptr;
    Frame* previous=current;current=nullptr;
    struct Restore {Frame* previous;Coverage* previousCoverage;void* previousInstance;~Restore(){current=previous;coverage=previousCoverage;traceInstance=previousInstance;}} restore{previous,previousCoverage,previousInstance};
    const uint32_t limit=particlediag::IsolatedEnd(true,first,end,elements,order);
    const char* reason="batch-shape";
    // Isolate all particle calls while enabled, so an incompatible leading native emitter cannot
    // absorb an eligible modern emitter in the stock single-texture batcher. Order is unchanged.
    try {
        if(limit==first+1&&first<end) {
            reason="identity";
            if(Identify(context,frame.id)) {
                reason="native-owner";
                if(assets::m2::IsNativeLoaded(frame.id.shared)) {
                    reason="source-missing";
                    frame.source=mat::SourceMaterials().FindModel(frame.id.owner);
                    const bool refractionTarget=RefractionAttributionTarget(frame);
                    frame.diagnosticOmit=omitBloodRefraction&&refractionTarget;
                    frame.bloodRefraction=bloodRefractionEnabled&&refractionTarget;
                    if(frame.diagnosticOmit || frame.bloodRefraction) {
                        current=&frame;reason=frame.diagnosticOmit?"diagnostic-refraction-omission":"blood-refraction";
                    } else if(frame.source) {
                        reason="source-decode";
                        const bool nativeSprites=DecodeRiftNativeSprites(riftNativeSpritesEnabled,RiftTarget(context),*frame.source,frame.id.index,frame.recipe);
                        frame.nativeSprite=DecodeNativeSprites(nativeSpriteEnabled,*frame.source,frame.id.index,frame.recipe);
                        frame.bloodSmoke=DecodeBloodSmokeApprox(bloodSmokeEnabled,bloodSmokeEnabled&&ExactParticlePath(frame,"spells\\cfx_deathknight_bloodboil_castworld.m2"),*frame.source,frame.id.index,frame.recipe);
                        frame.bloodPlague=DecodeBloodPlagueApprox(bloodPlagueEnabled,bloodPlagueEnabled&&ExactParticlePath(frame,"spells\\cfx_deathknight_bloodplague_statechest.m2"),*frame.source,frame.id.index,frame.recipe);
                        frame.nativeSprite=frame.nativeSprite||frame.bloodSmoke||frame.bloodPlague;
                        // Preserve the accepted exact-source paths when enabled.
                        if(!nativeSprites && !frame.nativeSprite) {
                            frame.spriteMaterial=DecodeSpriteMaterials(spriteMaterialsEnabled,*frame.source,frame.id.index,frame.recipe);
                            frame.nativeSprite=frame.spriteMaterial;
                        }
                        if(nativeSprites || frame.nativeSprite || Decode(*frame.source,frame.id.index,frame.recipe)) {
                            frame.viewTarget=IsViewTarget(frame);current=&frame;
                            reason=nativeSprites?"rift-native-sprites":frame.viewTarget?"ready":"view-target";
                        }
                    }
                }
            }
        }
    }catch(...){current=nullptr;reason="entry-exception";}
    if(RiftTarget(context) && Report(admissionReports,frame,reason,0)) {
        const auto* s=frame.source.get();
        WLOG_INFO("rift-diag-v1: admission owner=%p emitter=%u reason=%s admitted=%u source=%u flags=%#x features=%u blockers=%#x; diagnostics only",
            frame.id.owner,frame.id.index,reason,unsigned(current==&frame),unsigned(s!=nullptr),s?s->globalFlags:0,
            unsigned(s&&s->particleLayerFeaturesKnown),s?riftdiag::Blockers(*s,frame.id.index):0xffffffffu);
    }
    const auto result=original(context,edx,first,elements,order,limit);
    if(audited)WLOG_INFO("m2-particle-coverage: batch owner=%p emitter=%u first=%u end=%u limit=%u result=%u reason=%s target=%u build=%u captured=%u valid=%u dispatch=%u oneShot=%u ribbon=%u inactive=%u adapter=%u hidden=%u submitted=%u fallback=%u failed=%u hideAll=%u",frame.id.shared,frame.id.index,first,end,limit,result,reason,unsigned(frame.viewTarget),audit.build,frame.vertexCount,unsigned(frame.valid),audit.dispatch,audit.oneShot,audit.ribbon,audit.inactive,audit.adapter,audit.hidden,audit.submitted,audit.fallback,audit.failed,unsigned(hideAllEnabled));
    return result;
}
bool Active() noexcept{return enabled&&current;}
bool TraceEnabled() noexcept{return traceEnabled;}
bool TraceTarget() noexcept{return coverage!=nullptr;}
void* TraceInstance() noexcept{return traceInstance;}
void ObserveDIP(bool oneShot,bool ribbon) noexcept {
    if(!coverage)return;
    ++coverage->dispatch;
    if(oneShot)++coverage->oneShot;
    else if(ribbon)++coverage->ribbon;
    else if(!Active())++coverage->inactive;
}
long DrawDIP(void* device,int type,int base,unsigned min,unsigned vertices,unsigned start,unsigned primitives,DIPFn original) {
    if(!enabled||!current)return original(device,type,base,min,vertices,start,primitives);
    auto& frame=*current;
    if(coverage)++coverage->adapter;
    if(frame.bloodRefraction)return DrawBloodRefraction(device,type,base,min,vertices,start,primitives,original);
    // Native batch construction/simulation already ran. This diagnostic touches
    // no GPU state and does not enter layer preparation with an absent recipe.
    if(frame.diagnosticOmit) {
        if(Report(viewReports,frame,"refraction-omitted",0))
            WLOG_WARN("m2-refraction-attribution: omitted owner=%p emitter=%u vertices=%u primitives=%u; diagnostic only",frame.id.shared,frame.id.index,vertices,primitives);
        return D3D_OK;
    }
    // Diagnostic omission only: exact particle-only modern target and all source emitters
    // decoded above. No D3D calls/state changes, regardless of texture/shader readiness.
    // Native batch builder still executes once. Stock/non-target/invalid owners cannot enter.
    if(HideAllDraw(hideAllEnabled,frame.viewTarget)) {
        if(coverage)++coverage->hidden;
        return D3D_OK;
    }
    Prepared prepared;bool ready=false;
    try {
        if(device&&frame.valid&&QuadShape(frame.vertexCount,primitives)&&type==D3DPT_TRIANGLELIST&&
            vertices==frame.vertexCount&&primitives==frame.vertexCount/2&&frame.vertices.size()==size_t(vertices)*(frame.nativeStride+16))
            ready=Prepare(static_cast<IDirect3DDevice9*>(device),frame,prepared,base,min,start,primitives);
    }catch(...){ready=false;prepared.reason="prepare-exception";}
    if(!ready){
        if(coverage)++coverage->fallback;
        if(frame.viewTarget&&Report(viewReports,frame,"incomplete",frame.nativeKind))WLOG_WARN("m2-particle-view: incomplete owner=%p emitter=%u reason=%s; native fallback can contaminate isolation",frame.id.shared,frame.id.index,prepared.reason);
        if(!prepared.Restore()){enabled=false;WLOG_WARN("m2-particle-layers: prepare restore failed; disabled; restart client");return D3DERR_INVALIDCALL;}
        if(Report(skipReports,frame,prepared.reason,prepared.textureStage))WLOG_INFO("m2-particle-layers: fallback owner=%p emitter=%u reason=%s hr=%#lx stage=%u valid=%u capturedVertices=%u drawVertices=%u stride=%u type=%d base=%d min=%u start=%u primitives=%u",frame.id.shared,frame.id.index,prepared.reason,prepared.hr,prepared.textureStage,unsigned(frame.valid),frame.vertexCount,vertices,frame.nativeStride,type,base,min,start,primitives);
        return original(device,type,base,min,vertices,start,primitives);
    }
    // Only suppress after all native shape/shader/resource guards pass. No mutation of native
    // simulation or vertex data: the original batch builder still executes normally.
    if(HideEmitter(frame.viewTarget,viewEmitter,frame.id.index)) {
        if(coverage)++coverage->hidden;
        const bool restored=prepared.Restore();
        if(Report(viewReports,frame,"hidden",frame.nativeKind))WLOG_INFO("m2-particle-view: hidden owner=%p emitter=%u selected=%u mode=%u restored=%u",frame.id.shared,frame.id.index,viewEmitter,viewMode,unsigned(restored));
        if(!restored){enabled=false;WLOG_WARN("m2-particle-layers: view restore failed; disabled; restart client");return D3DERR_INVALIDCALL;}
        return D3D_OK;
    }
    const long result=original(device,type,0,0,vertices,0,primitives);
    if(coverage){++coverage->submitted;if(FAILED(result))++coverage->failed;}
    const bool restored=prepared.Restore();
    if(frame.viewTarget&&Report(viewReports,frame,"selected",frame.nativeKind))WLOG_INFO("m2-particle-view: selected owner=%p emitter=%u mode=%u vertices=%u restored=%u hr=%#lx",frame.id.shared,frame.id.index,viewMode,vertices,unsigned(restored),result);
    if(Report(appliedReports,frame,"applied",frame.nativeKind)) {
        if(frame.bloodPlague)WLOG_INFO("m2-blood-plague: applied=1 owner=%p emitter=%u restored=%u hr=%#lx",frame.id.shared,frame.id.index,unsigned(restored),result);
        if(frame.spriteMaterial)WLOG_INFO("m2-sprite-materials: applied=1 owner=%p emitter=%u metadata=%u blend=%u textures=%u,%u,%u restored=%u hr=%#lx",frame.id.shared,frame.id.index,unsigned(frame.source->spriteLayerKinds[frame.id.index]),unsigned(frame.source->particles[frame.id.index].Blend()),frame.recipe.indices[0],frame.recipe.indices[1],frame.recipe.indices[2],unsigned(restored),result);
        const float modulation=pm::SourceRGBModulationCandidate(modulationEnabled,frame.source->particles[frame.id.index].Flags());
        WLOG_INFO("m2-particle-layers: applied=1 owner=%p emitter=%u vertices=%u stride=%u layers=3 independentUV=1 rgbMult=%.3f alphaMult=%.3f nativeBase=%d nativeStart=%u rebased=1 uploadStateRestored=1 sourceRGBMod=%.0f effectiveRGBMult=%.3f nativeVS=%u material=%u restored=%u hr=%#lx bloodSmokeApprox=%u",frame.id.shared,frame.id.index,vertices,frame.nativeStride+16,frame.recipe.multipliers[0],frame.recipe.multipliers[1],base,start,modulation,frame.recipe.multipliers[0]*modulation,frame.nativeKind,unsigned(materialEnabled),unsigned(restored),result,unsigned(frame.bloodSmoke));
    }
    if(!restored){enabled=false;WLOG_WARN("m2-particle-layers: restore failed; disabled; restart client");}
    return result;
}
}
