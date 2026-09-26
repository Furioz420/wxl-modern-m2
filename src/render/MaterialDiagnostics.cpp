// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "MaterialDiagnostics.hpp"
#include "MaterialDiagnosticPolicy.hpp"
#include "ParticleDiagnosticPolicy.hpp"
#include "ShaderDiagnosticIdentity.hpp"
#include "../ExtensionApi.hpp"
#include "../compat/ModernM2.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"
#include <d3d9.h>
#include "shaders/CombinersModAddAlphaPs.h"
#include "shaders/CombinersModAddAlphaVs.h"
#include <cstring>
#include <cstdio>

namespace wxl::modern::materialdiag
{
    namespace mat = assets::m2::material;
    namespace fmt = wxl::structure::m2;
    namespace off = wxl::offsets::game::m2;
    namespace
    {
        bool enabled = false, includeNative = false;
        bool captureMeshShaders = false;
        particlediag::ShaderSet meshShaders;
        char pathFilter[256]{};
        ProbeBudget budget;
        // The existing draw context is render-thread scoped. Also suppress accidental recursion.
        bool reading = false;

        struct NativeStage
        {
            LookupValue texture, coord, weight, transform;
            bool descriptorValid = false;
            uint32_t type = 0;
            char filename[192]{};
        };
        struct Context
        {
            ProbeKey key;
            void* shared = nullptr;
            uint32_t batchCount = 0;
            fmt::M2Batch batch{};
            char path[512]{};
            float elementAlpha = 0;
            uint16_t blend = 0;
            bool hasBlend = false;
            uint16_t headerFlags = 0;
            bool hasHeaderFlags = false;
            NativeStage stages[4]{};
        };
        LookupValue LiveLookup(fmt::M2Array array, uint16_t base, uint32_t stage)
        {
            const uint32_t index = uint32_t(base) + stage;
            if (!array.offset || array.count > 0x10000 || index >= array.count) return {};
            const auto* values = reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(array.offset));
            return {true, values[index]};
        }
        // Keep SEH pointer reads in a POD-only function, outside shared_ptr/COM ownership scopes.
        bool ReadContext(void* raw, void* instance, Context& out) noexcept
        {
            if (!raw || !instance) return false;
            __try
            {
                const auto* dc = static_cast<const off::DrawContext*>(raw);
                if (dc->instance != instance || !dc->element) return false;
                out.shared = reinterpret_cast<void*>(static_cast<off::M2Instance*>(instance)->model);
                if (!out.shared) return false;
                wxl::game::m2::M2Model model(out.shared);
                const auto* md = model.GetHeader();
                const auto* skin = model.GetSkin();
                if (!md || !skin || !skin->batches || !skin->batchCount || skin->batchCount > 0x10000)
                    return false;
                const uint32_t batch = *reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(dc->element) + off::kOffElementBatchIndex);
                if (batch >= skin->batchCount) return false;
                out.key = {md, instance, skin, skin->batches, batch};
                out.batchCount = skin->batchCount;
                out.batch = skin->batches[batch];
                if(md->materials.offset&&md->materials.count<=0x10000&&out.batch.materialIndex<md->materials.count) {
                    out.headerFlags=reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(md->materials.offset))[out.batch.materialIndex*2];
                    out.hasHeaderFlags=true;
                }
                out.elementAlpha = *reinterpret_cast<const float*>(
                    static_cast<const uint8_t*>(dc->element) + off::kOffElementAlpha);
                if (dc->material)
                {
                    out.blend = static_cast<const off::Material*>(dc->material)->blend;
                    out.hasBlend = true;
                }
                const char* path = model.GetPathStem();
                if (path)
                    for (size_t n = 0; n + 1 < sizeof(out.path) &&
                         n < sizeof(off::M2Model::pathStem) && path[n]; ++n) out.path[n] = path[n];
                for (uint32_t stage = 0; stage < 4 && stage < out.batch.textureCount; ++stage)
                {
                    out.stages[stage].texture = LiveLookup(md->textureCombos, out.batch.textureComboIndex, stage);
                    out.stages[stage].coord = LiveLookup(md->textureUnitLookup, out.batch.textureCoordComboIndex, stage);
                    out.stages[stage].weight = LiveLookup(md->textureWeightCombos, out.batch.textureWeightComboIndex, stage);
                    out.stages[stage].transform = LiveLookup(md->textureTransformCombos, out.batch.textureTransformComboIndex, stage);
                    auto& s = out.stages[stage];
                    if (s.texture.available && md->textures.offset && md->textures.count <= 0x10000 &&
                        s.texture.value < md->textures.count)
                    {
                        const auto* textures = reinterpret_cast<const fmt::M2Texture*>(static_cast<uintptr_t>(md->textures.offset));
                        const auto& texture = textures[s.texture.value];
                        s.descriptorValid = true;
                        s.type = texture.type;
                        if (texture.filename.count && texture.filename.offset)
                        {
                            const auto* filename = reinterpret_cast<const char*>(static_cast<uintptr_t>(texture.filename.offset));
                            for (size_t n = 0; n + 1 < sizeof(s.filename) && n < texture.filename.count && filename[n]; ++n)
                                s.filename[n] = filename[n];
                        }
                    }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        template<class T> struct ComRead
        {
            T* pointer = nullptr;
            ~ComRead() { if (pointer) pointer->Release(); }
        };
        const char* BindingState(HRESULT hr, const void* pointer) noexcept
        {
            return FAILED(hr) ? "unavailable" : (pointer ? "bound" : "unbound");
        }
        int RawValue(LookupValue value) noexcept { return value.available ? int(value.value) : -1; }

        void ReportSource(uint32_t id, const Context& context)
        {
            auto source = mat::SourceMaterials().Find(context.key.owner, context.key.skin,
                context.key.batches, context.batchCount);
            if (!source || !source->model || context.key.batch >= source->outputs.size())
            {
                WLOG_INFO("m2-material-probe: id=%u source=unavailable (not evidence of a DB2 failure)", id);
                return;
            }
            const auto& origin = source->outputs[context.key.batch];
            if (origin.sourceBatch >= source->batches.size()) return;
            const auto& batch = source->batches[origin.sourceBatch];
            const auto classification = source->ClassifySource(origin.sourceBatch);
            const bool hasMaterial = batch.materialIndex < source->model->materials.size();
            const auto flags = hasMaterial ? source->model->materials[batch.materialIndex] : mat::SourceRenderFlags{};
            WLOG_INFO("m2-material-probe: id=%u sourceBatch=%u piece=%u split=%u parked=%u version=%u"
                " profile=%u classification=%s sourceShader=%#x sourceTextures=%u material=%u"
                " rawMaterialAvailable=%u rawFlags=%#x rawBlend=%u color=%u layer=%u",
                id, origin.sourceBatch, unsigned(origin.piece), unsigned(origin.split), unsigned(origin.parked),
                source->model->innerVersion, unsigned(source->model->profile), mat::ReasonName(classification.reason),
                unsigned(batch.shaderId), unsigned(batch.textureCount), unsigned(batch.materialIndex),
                unsigned(hasMaterial), unsigned(flags.flags), unsigned(flags.blend), unsigned(batch.colorIndex),
                unsigned(batch.materialLayer));
            for (uint32_t stage = 0; stage < 4 && stage < batch.textureCount; ++stage)
            {
                const auto s = InspectStage(*source->model, batch, stage);
                WLOG_INFO("m2-material-probe: id=%u sourceStage=%u textureIndex=%d descriptorValid=%u"
                    " type=%u flags=%#x txidAvailable=%u txid=%u coordRaw=%d weightRaw=%d transformRaw=%d",
                    id, stage, RawValue(s.texture), unsigned(s.textureValid), s.type, s.flags,
                    unsigned(s.txidAvailable), s.txid, RawValue(s.coord), RawValue(s.weight), RawValue(s.transform));
            }
        }

        template<class Shader> void ReportMeshShader(uint32_t id, const char* kind, Shader* shader)
        {
            particlediag::ShaderBytes bytes;
            bool added = false;
            const unsigned dump = particlediag::ReadShader(shader, bytes) ? meshShaders.Insert(bytes, added) : 0;
            WLOG_INFO("m2-mesh-input: id=%u shader=%s dumpId=%u bytes=%u new=%u", id, kind, dump, bytes.size, unsigned(added));
            if (added) for (unsigned at = 0; at < bytes.size; at += 32) {
                char hex[65]{};
                const unsigned count = (bytes.size-at < 32) ? bytes.size-at : 32;
                for (unsigned n = 0; n < count; ++n)
                    sprintf_s(hex+2*n, sizeof(hex)-2*n, "%02x", unsigned(bytes.data[at+n]));
                WLOG_INFO("m2-mesh-shader: dumpId=%u offset=%u hex=%s", dump, at, hex);
            }
        }
        void ReportDevice(uint32_t id, IDirect3DDevice9* device, bool meshCapture = false)
        {
            ComRead<IDirect3DVertexShader9> vs;
            ComRead<IDirect3DPixelShader9> ps;
            ComRead<IDirect3DVertexDeclaration9> decl;
            ComRead<IDirect3DVertexBuffer9> stream;
            const HRESULT vsHr = device->GetVertexShader(&vs.pointer);
            const HRESULT psHr = device->GetPixelShader(&ps.pointer);
            const HRESULT declHr = device->GetVertexDeclaration(&decl.pointer);
            UINT offset = 0, stride = 0;
            const HRESULT streamHr = device->GetStreamSource(0, &stream.pointer, &offset, &stride);
            D3DDEVICE_CREATION_PARAMETERS creation{};
            const HRESULT creationHr = device->GetCreationParameters(&creation);
            WLOG_INFO("m2-material-probe: id=%u device=%p creationHr=%#lx creationFlags=%#lx"
                " vs=%p vsState=%s vsHr=%#lx ps=%p psState=%s psHr=%#lx"
                " decl=%p declHr=%#lx stream0=%p streamHr=%#lx offset=%u stride=%u",
                id, device, creationHr, creation.BehaviorFlags,
                vs.pointer, BindingState(vsHr, vs.pointer), vsHr,
                ps.pointer, BindingState(psHr, ps.pointer), psHr, decl.pointer, declHr,
                stream.pointer, streamHr, offset, stride);
            const auto vsIdentity = InspectShader(SUCCEEDED(vsHr) ? vs.pointer : nullptr,
                kCombinersModAddAlphaVs, sizeof(kCombinersModAddAlphaVs));
            const auto psIdentity = InspectShader(SUCCEEDED(psHr) ? ps.pointer : nullptr,
                kCombinersModAddAlphaPs, sizeof(kCombinersModAddAlphaPs));
            WLOG_INFO("m2-material-probe: id=%u shader=VS bytecode=%s bytes=%u fnv1a=%#x"
                " exactWxlAddAlpha=%u sizeHr=%#x dataHr=%#x", id, ShaderReadName(vsIdentity.state),
                vsIdentity.bytes, vsIdentity.hash, unsigned(vsIdentity.exactKnown),
                unsigned(vsIdentity.sizeHr), unsigned(vsIdentity.dataHr));
            WLOG_INFO("m2-material-probe: id=%u shader=PS bytecode=%s bytes=%u fnv1a=%#x"
                " exactWxlAddAlpha=%u sizeHr=%#x dataHr=%#x", id, ShaderReadName(psIdentity.state),
                psIdentity.bytes, psIdentity.hash, unsigned(psIdentity.exactKnown),
                unsigned(psIdentity.sizeHr), unsigned(psIdentity.dataHr));
            // Only the opt-in ordinary-mesh caller enables this. General particle GPU
            // probes must not consume the independent eight-program exact-byte budget.
            if (meshCapture) {
                ReportMeshShader(id, "VS", SUCCEEDED(vsHr) ? vs.pointer : nullptr);
                ReportMeshShader(id, "PS", SUCCEEDED(psHr) ? ps.pointer : nullptr);
                // Raw low registers include the known native UV/fog/model slots. Bone
                // palettes above c33 are deliberately not dumped; no VB reads or writes.
                for (unsigned reg = 0; reg < 34; ++reg) {
                    float v[4]{};
                    const HRESULT hr = device->GetVertexShaderConstantF(reg, v, 1);
                    WLOG_INFO("m2-mesh-input: id=%u vsC=%u hr=%#lx value=(%.9g,%.9g,%.9g,%.9g)", id, reg, hr, v[0],v[1],v[2],v[3]);
                }
                for (unsigned reg = 0; reg < 8; ++reg) {
                    float v[4]{};
                    const HRESULT hr = device->GetPixelShaderConstantF(reg, v, 1);
                    WLOG_INFO("m2-mesh-input: id=%u psC=%u hr=%#lx value=(%.9g,%.9g,%.9g,%.9g)", id, reg, hr, v[0],v[1],v[2],v[3]);
                }
            }
            if (SUCCEEDED(declHr) && decl.pointer)
            {
                D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
                UINT count = MAXD3DDECLLENGTH + 1;
                const HRESULT hr = decl.pointer->GetDeclaration(elements, &count);
                WLOG_INFO("m2-material-probe: id=%u declarationHr=%#lx elements=%u (report cap=16)", id, hr, count);
                if (SUCCEEDED(hr))
                    for (UINT n = 0; n < count && n < 16 && elements[n].Stream != 0xFF; ++n)
                    {
                        const auto& e = elements[n];
                        WLOG_INFO("m2-material-probe: id=%u declElement=%u stream=%u offset=%u type=%u usage=%u index=%u",
                            id, n, unsigned(e.Stream), unsigned(e.Offset), unsigned(e.Type), unsigned(e.Usage), unsigned(e.UsageIndex));
                    }
            }
            for (DWORD stage = 0; stage < 4; ++stage)
            {
                ComRead<IDirect3DBaseTexture9> texture;
                const HRESULT hr = device->GetTexture(stage, &texture.pointer);
                D3DSURFACE_DESC desc{};
                HRESULT descHr = E_FAIL;
                unsigned type = 0;
                if (SUCCEEDED(hr) && texture.pointer)
                {
                    type = unsigned(texture.pointer->GetType());
                    if (type == D3DRTYPE_TEXTURE)
                        descHr = static_cast<IDirect3DTexture9*>(texture.pointer)->GetLevelDesc(0, &desc);
                    else if (type == D3DRTYPE_CUBETEXTURE)
                        descHr = static_cast<IDirect3DCubeTexture9*>(texture.pointer)->GetLevelDesc(0, &desc);
                }
                WLOG_INFO("m2-material-probe: id=%u gpuStage=%lu texture=%p state=%s hr=%#lx type=%u"
                    " descHr=%#lx width=%u height=%u format=%u pool=%u",
                    id, stage, texture.pointer, BindingState(hr, texture.pointer), hr, type,
                    descHr, desc.Width, desc.Height, unsigned(desc.Format), unsigned(desc.Pool));
                DWORD u = 0, v = 0, min = 0, mag = 0, mip = 0, srgb = 0;
                const HRESULT hu = device->GetSamplerState(stage, D3DSAMP_ADDRESSU, &u);
                const HRESULT hv = device->GetSamplerState(stage, D3DSAMP_ADDRESSV, &v);
                const HRESULT hn = device->GetSamplerState(stage, D3DSAMP_MINFILTER, &min);
                const HRESULT hg = device->GetSamplerState(stage, D3DSAMP_MAGFILTER, &mag);
                const HRESULT hm = device->GetSamplerState(stage, D3DSAMP_MIPFILTER, &mip);
                const HRESULT hs = device->GetSamplerState(stage, D3DSAMP_SRGBTEXTURE, &srgb);
                WLOG_INFO("m2-material-probe: id=%u sampler=%lu u=%lu/%#lx v=%lu/%#lx min=%lu/%#lx"
                    " mag=%lu/%#lx mip=%lu/%#lx srgb=%lu/%#lx (value/hr)", id, stage, u, hu, v, hv, min, hn, mag, hg, mip, hm, srgb, hs);
                DWORD color = 0, alpha = 0, coord = 0, transform = 0;
                const HRESULT hc = device->GetTextureStageState(stage, D3DTSS_COLOROP, &color);
                const HRESULT ha = device->GetTextureStageState(stage, D3DTSS_ALPHAOP, &alpha);
                const HRESULT ht = device->GetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, &coord);
                const HRESULT hx = device->GetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, &transform);
                WLOG_INFO("m2-material-probe: id=%u fixedStage=%lu colorOp=%lu/%#lx alphaOp=%lu/%#lx coord=%lu/%#lx"
                    " transformFlags=%lu/%#lx (value/hr; not programmable shader equations)", id, stage, color, hc, alpha, ha, coord, ht, transform, hx);
            }
            for (const auto state : {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
                D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
                D3DRS_ZWRITEENABLE, D3DRS_FOGENABLE, D3DRS_LIGHTING})
            {
                DWORD value = 0;
                const HRESULT hr = device->GetRenderState(state, &value);
                WLOG_INFO("m2-material-probe: id=%u renderState=%u value=%lu hr=%#lx", id, unsigned(state), value, hr);
            }
            // Raw register samples only: do not assign semantics without the active shader ABI.
            for (UINT first : {0u, 31u})
            {
                float values[12]{};
                const HRESULT hr = device->GetVertexShaderConstantF(first, values, 3);
                WLOG_INFO("m2-material-probe: id=%u vsConstantsFirst=%u hr=%#lx"
                    " c0=(%g,%g,%g,%g) c1=(%g,%g,%g,%g) c2=(%g,%g,%g,%g)", id, first, hr,
                    values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                    values[8], values[9], values[10], values[11]);
            }
            float pixels[4]{};
            const HRESULT ph = device->GetPixelShaderConstantF(0, pixels, 1);
            WLOG_INFO("m2-material-probe: id=%u psConstant0=(%g,%g,%g,%g) hr=%#lx", id,
                pixels[0], pixels[1], pixels[2], pixels[3], ph);
        }
    }

    void ReportGpu(void* device, unsigned id) noexcept
    {
        if (!device) return;
        try { ReportDevice(id, static_cast<IDirect3DDevice9*>(device)); }
        catch (...) {}
    }

    void Initialize() noexcept
    {
        try
        {
            enabled = wxl_modern_m2::ConfigBool("WXL_M2_MATERIAL_PROBE", false);
            captureMeshShaders = enabled && wxl_modern_m2::ConfigBool("WXL_M2_MESH_SHADER_CAPTURE", false);
            if (!enabled) return;
            includeNative = wxl_modern_m2::ConfigBool("WXL_M2_MATERIAL_PROBE_NATIVE", false);
            pathFilter[0] = '\0';
            wxl_modern_m2::ConfigRaw("WXL_M2_MATERIAL_PROBE_PATH", pathFilter, sizeof(pathFilter));
            const auto limit = wxl_modern_m2::ConfigU32("WXL_M2_MATERIAL_PROBE_LIMIT", 32, 1, 64);
            const auto samples = wxl_modern_m2::ConfigU32("WXL_M2_MATERIAL_PROBE_SAMPLES", 2, 1, 4);
            budget = ProbeBudget(limit, samples, 250);
            meshShaders = {};
            WLOG_INFO("m2-material-probe: enabled path='%s' native=%u limit=%u samples=%u intervalMs=250; read-only ordinary M2 DIP only",
                pathFilter, unsigned(includeNative), limit, samples);
            if (captureMeshShaders) WLOG_INFO("m2-mesh-input: enabled; read-only ordinary mesh shaders and low constants; 8 exact programs, 4096 bytes each; no shader replacement");
        }
        catch (...) { enabled = false; }
    }

    void BeforeDraw(void* device, void* drawContext, void* instance,
                    unsigned startIndex, unsigned primitiveCount) noexcept
    {
        if (!enabled || !device || !instance || reading || budget.Exhausted()) return;
        reading = true;
        try
        {
            Context context{};
            if (ReadContext(drawContext, instance, context) && MatchesPath(context.path, pathFilter))
            {
                const bool modern = assets::m2::IsNativeLoaded(context.shared);
                if ((modern || includeNative) && budget.Take(context.key, GetTickCount()))
                {
                    const uint32_t id = budget.Total();
                    WLOG_INFO("m2-material-probe: id=%u path='%s' modern=%u instance=%p owner=%p skin=%p"
                        " batch=%u nativeShader=%#x nativeTextures=%u material=%u liveBlendAvailable=%u liveBlend=%u"
                        " elementAlpha=%g start=%u primitives=%u phase=before-ordinary-DIP",
                        id, context.path, unsigned(modern), instance, context.key.owner, context.key.skin,
                        context.key.batch, unsigned(context.batch.shaderId), unsigned(context.batch.textureCount),
                        unsigned(context.batch.materialIndex), unsigned(context.hasBlend), unsigned(context.blend),
                        context.elementAlpha, startIndex, primitiveCount);
                    ReportSource(id, context);
                    WLOG_INFO("m2-material-probe: id=%u liveHeaderFlags=%#x available=%u",id,unsigned(context.headerFlags),unsigned(context.hasHeaderFlags));
                    for (uint32_t stage = 0; stage < 4 && stage < context.batch.textureCount; ++stage)
                    {
                        const auto& s = context.stages[stage];
                        WLOG_INFO("m2-material-probe: id=%u nativeStage=%u textureIndex=%d coordRaw=%d weightRaw=%d transformRaw=%d"
                            " descriptorValid=%u type=%u filename='%s' (path cap=191)",
                            id, stage, RawValue(s.texture), RawValue(s.coord), RawValue(s.weight), RawValue(s.transform),
                            unsigned(s.descriptorValid), s.type, s.filename);
                    }
                    ReportDevice(id, static_cast<IDirect3DDevice9*>(device), captureMeshShaders);
                    if (budget.Exhausted()) WLOG_INFO("m2-material-probe: sample budget exhausted; restart to rearm");
                }
            }
        }
        catch (...) {} // diagnostic allocation/logging failure must not suppress the native draw
        reading = false;
    }
}
