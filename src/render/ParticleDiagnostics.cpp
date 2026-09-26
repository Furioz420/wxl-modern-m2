// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "ParticleDiagnostics.hpp"
#include "ParticleLayers.hpp"
#include "ParticleDiagnosticPolicy.hpp"
#include "MaterialDiagnosticPolicy.hpp"
#include "MaterialDiagnostics.hpp"
#include "../ExtensionApi.hpp"
#include "../compat/ModernM2.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"
#include <d3d9.h>
#include <cstdio>

namespace wxl::modern::particlediag
{
    namespace off = wxl::offsets::game::m2;
    namespace mat = assets::m2::material;
    namespace fmt = wxl::structure::m2;
    namespace diag = materialdiag;
    namespace
    {
        bool enabled = false, isolate = false, includeNative = false, reading = false;
        char filter[256]{};
        diag::ProbeBudget budget;
        ShaderSet shaders;
        DrawFn originalDraw = nullptr;
        unsigned entryReports = 0;
        struct Context
        {
            void* shared = nullptr;
            const fmt::M2Header* owner = nullptr;
            void* instance = nullptr;
            void* emitter = nullptr;
            uintptr_t record = 0, textureHandle = 0;
            uint32_t index = 0, count = 0, live = 0, nativeBlend = 0, nativeFlags = 0;
            uint32_t gpuSamples = 0;
            uint16_t nativeTexture = 0;
            float alpha = 0;
            bool recordValid = false, isolated = false;
            char path[276]{};
        };
        Context* active = nullptr; // scoped to the render-thread's native DrawParticle call
        template<class T> T Read(const void* p, size_t at) noexcept
        {
            T value;
            std::memcpy(&value, static_cast<const uint8_t*>(p) + at, sizeof(value));
            return value;
        }
        bool ReadContext(void* raw, Context& out) noexcept
        {
            if (!raw) return false;
            __try
            {
                const auto* dc = static_cast<const off::DrawContext*>(raw);
                if (!dc->instance || !dc->element || Read<uint32_t>(dc->element, 0) != 4 ||
                    Read<void*>(dc->element, 4) != dc->instance) return false;
                out.instance = dc->instance;
                out.emitter = Read<void*>(dc->element, 0x18);
                out.alpha = Read<float>(dc->element, off::kOffElementAlpha);
                out.shared = reinterpret_cast<void*>(static_cast<off::M2Instance*>(dc->instance)->model);
                if (!out.emitter || !out.shared) return false;
                wxl::game::m2::M2Model model(out.shared);
                out.owner = model.GetHeader();
                if (!out.owner) return false;
                const char* path = model.GetPathStem();
                if (path) for (size_t n=0; n+1 < sizeof(out.path) && path[n]; ++n) out.path[n] = path[n];
                out.count = out.owner->particleEmitters.count;
                const uintptr_t head = Read<uintptr_t>(out.emitter, off::kOffEmitterHeadCellBlock);
                out.recordValid = EmitterIndex(out.owner->particleEmitters.offset, out.count, head, out.index);
                if (out.recordValid)
                {
                    out.record = head - off::kParticleRecHeadCells;
                    out.nativeTexture = Read<uint16_t>(reinterpret_cast<void*>(out.record), 0x16);
                }
                out.live = Read<uint32_t>(out.emitter, off::kOffEmitterLiveCount);
                out.nativeBlend = Read<uint32_t>(out.emitter, off::kOffEmitterBlendState);
                out.nativeFlags = Read<uint32_t>(out.emitter, off::kOffEmitterFlags);
                out.textureHandle = Read<uintptr_t>(out.emitter, 0x128);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool Filename(const Context& ctx, uint16_t index, char (&out)[192]) noexcept
        {
            __try
            {
                if (!ctx.owner->textures.offset || ctx.owner->textures.count > 65536 || index >= ctx.owner->textures.count) return false;
                const auto* textures = reinterpret_cast<const fmt::M2Texture*>(uintptr_t(ctx.owner->textures.offset));
                const auto& texture = textures[index];
                if (!texture.filename.offset || !texture.filename.count) return false;
                const auto* name = reinterpret_cast<const char*>(uintptr_t(texture.filename.offset));
                for (size_t n=0; n+1 < sizeof(out) && n < texture.filename.count && name[n]; ++n) out[n] = name[n];
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        void Source(uint32_t id, const Context& ctx)
        {
            const auto source = mat::SourceMaterials().FindModel(ctx.owner);
            if (!source || !ctx.recordValid || source->particleCapture != mat::ParticleCapture::Captured ||
                source->particles.size() != ctx.count || ctx.index >= source->particles.size())
            {
                WLOG_INFO("m2-particle-probe: id=%u source=unavailable captureStatus=%u; not proof of missing textures",
                    id, source ? unsigned(source->particleCapture) : 255u);
                return;
            }
            const auto& p = source->particles[ctx.index];
            WLOG_INFO("m2-particle-probe: id=%u version=%u sourceFlags=%#x sourceBlend=%u multi=%u packed=%u"
                " scaleRaw=(%u,%u) scrollRaw=(%u,%u,%u,%u,%u,%u,%u,%u)", id, source->innerVersion,
                p.Flags(), unsigned(p.Blend()), unsigned(p.MultiTexture()), unsigned(p.U16(0x16)),
                unsigned(p.bytes[0x2c]), unsigned(p.bytes[0x2d]), unsigned(p.U16(0x1dc)), unsigned(p.U16(0x1de)),
                unsigned(p.U16(0x1e0)), unsigned(p.U16(0x1e2)), unsigned(p.U16(0x1e4)), unsigned(p.U16(0x1e6)),
                unsigned(p.U16(0x1e8)), unsigned(p.U16(0x1ea)));
            for (unsigned stage=0; stage < (p.MultiTexture() ? 3u : 1u); ++stage)
            {
                const uint16_t index = p.Texture(stage);
                char name[192]{};
                const bool named = Filename(ctx, index, name);
                const bool txid = index < source->textureFileDataIds.size();
                WLOG_INFO("m2-particle-probe: id=%u sourceStage=%u textureIndex=%u txidAvailable=%u txid=%u"
                    " liveFilenameAvailable=%u filename='%s'; not a sampler-binding claim", id, stage, unsigned(index),
                    unsigned(txid), txid ? source->textureFileDataIds[index] : 0, unsigned(named), name);
            }
        }
        template<class T> struct ComRead { T* value=nullptr; ~ComRead() { if(value) value->Release(); } };
        template<class T> void DumpShader(uint32_t id, const char* kind, T* shader)
        {
            ShaderBytes bytes;
            if (!ReadShader(shader, bytes))
            {
                WLOG_INFO("m2-particle-probe: id=%u shader=%s dump=unavailable-or-over-budget", id, kind);
                return;
            }
            bool added = false;
            const unsigned dump = shaders.Insert(bytes, added);
            WLOG_INFO("m2-particle-probe: id=%u shader=%s dumpId=%u bytes=%u new=%u", id, kind, dump, bytes.size, unsigned(added));
            if (!added) return;
            for (unsigned offset=0; offset < bytes.size; offset+=32)
            {
                char hex[65]{};
                const unsigned count = bytes.size-offset < 32 ? bytes.size-offset : 32;
                for (unsigned n=0;n<count;++n) sprintf_s(hex+2*n, sizeof(hex)-2*n, "%02x", unsigned(bytes.data[offset+n]));
                WLOG_INFO("m2-particle-shader: dumpId=%u offset=%u hex=%s", dump, offset, hex);
            }
        }
        uint32_t __fastcall DiagnosticDraw(void* ctx, void* edx, uint32_t first, void* elements, const uint32_t* order, uint32_t end)
        {
            return Draw(originalDraw, ctx, edx, first, elements, order, end);
        }
        uint32_t __fastcall Hook(void* ctx, void* edx, uint32_t first, void* elements, const uint32_t* order, uint32_t end)
        {
            return particlelayers::AroundDraw(&DiagnosticDraw,ctx,edx,first,elements,order,end);
        }
    }
    void Initialize() noexcept
    {
        try
        {
            const bool layers = particlelayers::Initialize();
            enabled = wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_PROBE", false);
            if (!enabled && !layers) return;
            filter[0] = 0;
            wxl_modern_m2::ConfigRaw("WXL_M2_PARTICLE_PROBE_PATH", filter, sizeof(filter));
            includeNative = wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_PROBE_NATIVE", false);
            isolate = wxl_modern_m2::ConfigBool("WXL_M2_PARTICLE_PROBE_ISOLATE", false);
            budget = diag::ProbeBudget(wxl_modern_m2::ConfigU32("WXL_M2_PARTICLE_PROBE_LIMIT", 32, 1, 64), 3, 250);
            shaders = ShaderSet{};
            entryReports = 0;
            if (!originalDraw && !wxl_modern_m2::HookAttachByName("M2.DrawParticleBatch", &Hook, &originalDraw))
            {
                enabled = false;
                WLOG_WARN("m2-particle-probe: hook unavailable; disabled");
                return;
            }
            if(enabled) WLOG_INFO("m2-particle-probe: enabled path='%s' native=%u isolate=%u; GPU reads only; isolation changes batching only",
                filter, unsigned(includeNative), unsigned(isolate));
        }
        catch (...) { enabled = false; }
    }
    uint32_t Draw(DrawFn original, void* raw, void* edx, uint32_t first,
                  void* elements, const uint32_t* order, uint32_t end)
    {
        if (!enabled || reading || budget.Exhausted()) return original(raw, edx, first, elements, order, end);
        Context ctx{};
        Context* previous = active;
        active = nullptr; // nested nonmatching draws cannot inherit an outer attribution
        const uint32_t limited = IsolatedEnd(isolate, first, end, elements, order);
        if (ReadContext(raw, ctx) && diag::MatchesPath(ctx.path, filter) &&
            (includeNative || assets::m2::IsNativeLoaded(ctx.shared)))
        {
            ctx.isolated = isolate && elements && order && first < end && end <= 0x10000;
            active = &ctx;
            if (entryReports < 12)
            {
                ++entryReports;
                WLOG_INFO("m2-particle-probe: entry=%u path='%s' emitter=%p first=%u end=%u submittedEnd=%u",
                    entryReports, ctx.path, ctx.emitter, first, end, limited);
            }
        }
        struct RestoreActive { Context* saved; ~RestoreActive() { active=saved; } } restore{previous};
        return original(raw, edx, first, elements, order, limited);
    }
    void BeforeDIP(void* rawDevice, unsigned start, unsigned primitives) noexcept
    {
        if (!enabled || !rawDevice || !active || reading || budget.Exhausted() || active->gpuSamples) return;
        reading = true;
        try
        {
            auto& ctx = *active;
            const diag::ProbeKey key{ctx.owner, ctx.instance, ctx.emitter, reinterpret_cast<void*>(ctx.record), ctx.index};
            if (budget.Take(key, GetTickCount()))
            {
                ++ctx.gpuSamples;
                const uint32_t id = budget.Total(), gpuId = 1000 + id;
                WLOG_INFO("m2-particle-probe: id=%u path='%s' modern=%u instance=%p owner=%p emitter=%p"
                    " emitterIndex=%u recordValid=%u live=%u nativeTexture=%u nativeBlend=%u nativeFlags=%#x"
                    " textureHandle=%p elementAlpha=%g isolated=%u gpuReportId=%u start=%u primitives=%u",
                    id, ctx.path, unsigned(assets::m2::IsNativeLoaded(ctx.shared)), ctx.instance, ctx.owner, ctx.emitter,
                    ctx.index, unsigned(ctx.recordValid), ctx.live, unsigned(ctx.nativeTexture), ctx.nativeBlend, ctx.nativeFlags,
                    reinterpret_cast<void*>(ctx.textureHandle), ctx.alpha, unsigned(ctx.isolated), gpuId, start, primitives);
                Source(id, ctx);
                materialdiag::ReportGpu(rawDevice, gpuId);
                auto* device = static_cast<IDirect3DDevice9*>(rawDevice);
                ComRead<IDirect3DVertexShader9> vs;
                ComRead<IDirect3DPixelShader9> ps;
                if (SUCCEEDED(device->GetVertexShader(&vs.value))) DumpShader(id, "VS", vs.value);
                if (SUCCEEDED(device->GetPixelShader(&ps.value))) DumpShader(id, "PS", ps.value);
            }
        }
        catch (...) {} // failure to allocate/log cannot suppress the native particle draw
        reading = false;
    }
}
