// M2 ground-shadow draw: diagnosis of, and in-place guard against, camera-locked shadow sections.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "ShadowSpace.hpp"
#include "ShadowTracePolicy.hpp"
#include "ShadowIndexPolicy.hpp"
#include "ShadowBatchLimit.hpp"
#include "ShaderDiagnosticIdentity.hpp"

#include "../ExtensionApi.hpp"
#include "../compat/BoneBudget.hpp"
#include "../compat/ModernM2.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    namespace off = wxl::offsets::game::m2;
    namespace fmt = wxl::structure::m2;

    bool              g_armed = false;
    std::atomic<bool> g_enabled{ true };

    std::atomic<uint32_t> g_statDraws{ 0 };
    std::atomic<uint32_t> g_statInflZero{ 0 };
    std::atomic<uint32_t> g_statInflFixed{ 0 };
    std::atomic<uint32_t> g_statOverride{ 0 };
    std::atomic<uint32_t> g_statStaleAnim{ 0 };
    std::atomic<uint32_t> g_statPaletteMismatch{ 0 };
    std::atomic<uint32_t> g_statFaults{ 0 };

    bool g_traceEnabled = false;
    uint32_t g_indexMode = wxl_modern_m2::shadowindex::kDefaultMode; // 0 off, 1 observe, 2 repair
    uint32_t g_indexReports = 0, g_indexFaults = 0;
    thread_local wxl::runtime::m2shadow::TraceContext* g_traceContext = nullptr;
    struct TraceEntry
    {
        void* instance = nullptr;
        void* shared = nullptr;
        uint16_t influences = 0;
        wxl_modern_m2::shadowtrace::Gate gate;
    };
    // D3D draw-thread only. No retained COM objects or model dereferences outside the draw.
    TraceEntry g_traceEntries[64]{};
    uint32_t g_traceReports = 0;
    uint32_t g_traceShaders[8]{}, g_traceShaderCount = 0;

    struct IndexCandidate
    {
        wxl_modern_m2::shadowindex::Input input;
        void* instance = nullptr;
        uint32_t full = 0;
        char path[260]{};
    };
    bool ReadIndexCandidate(int primitiveType, unsigned startIndex, unsigned primitiveCount, IndexCandidate& out)
    {
        const auto* ctx=g_traceContext;
        if (!ctx || !ctx->drawList || !ctx->section) return false;
        const auto* runs=*static_cast<const uint32_t* const*>(ctx->drawList);
        if (!runs) return false;
        const auto* run=runs+ctx->drawIndex*off::kShadowRunStride;
        const uint32_t runCount=run[off::kShadowRunCountField];
        if (!runCount) return false;
        auto* inst=reinterpret_cast<off::M2Instance*>(run[0]);
        if (!inst || !inst->model) return false;
        void* shared=reinterpret_cast<void*>(inst->model);
        // A capacity-one scope preserves the native multi-run cursor while each DIP uses
        // a single instance. Never reinterpret an ordinary multi-instance buffer.
        const auto capacity=wxl_modern_m2::shadowbatch::active;
        if (runCount!=1 && !(capacity.model==shared && capacity.limit==1)) return false;
        if (!wxl::modern::assets::m2::IsNativeLoaded(shared)) return false;
        const wxl::game::m2::M2Model model(shared);
        const char* path=model.GetPathStem();
        if (!path || !wxl::modern::assets::common::bones::UsesExtendedIndexStart(path)) return false;
        const auto* skin=model.GetSkin();
        const auto* section=static_cast<const fmt::M2SkinSection*>(ctx->section);
        if (!skin || !skin->indices || !section->level) return false;
        out.input={true,true,primitiveType==D3DPT_TRIANGLELIST,1,section->level,section->indexStart,
            section->indexCount,startIndex,primitiveCount,skin->indexCount};
        out.full=wxl_modern_m2::shadowindex::Candidate(out.input);
        if (out.full==startIndex) return false;
        out.instance=inst;
        strncpy_s(out.path,path,_TRUNCATE);
        return true;
    }

    bool BoundIndexBufferFits(IDirect3DDevice9* device, const IndexCandidate& candidate, uint32_t& bytes, HRESULT& hr)
    {
        IDirect3DIndexBuffer9* buffer=nullptr;
        hr=device->GetIndices(&buffer);
        D3DINDEXBUFFER_DESC desc{};
        if (SUCCEEDED(hr)) hr=buffer?buffer->GetDesc(&desc):E_FAIL;
        if (buffer) buffer->Release();
        bytes=desc.Size;
        const uint32_t stride=desc.Format==D3DFMT_INDEX16?2u:desc.Format==D3DFMT_INDEX32?4u:0u;
        return SUCCEEDED(hr) && wxl_modern_m2::shadowindex::FitsBuffer(candidate.full,
            candidate.input.sectionCount,desc.Size,stride);
    }

    void LogMatrix(uint32_t id, const char* label, const float* m, unsigned rows)
    {
        for (unsigned r = 0; r < rows; ++r)
            WLOG_INFO("shadow-space-v1: id=%u %s[%u]=%.9g,%.9g,%.9g,%.9g",
                      id, label, r, m[4*r], m[4*r+1], m[4*r+2], m[4*r+3]);
    }

    // All fallible native-memory reads happen here, before acquiring any COM references.
    void TraceDraw(IDirect3DDevice9* device, unsigned startIndex, unsigned primitiveCount)
    {
        auto& ctx = *g_traceContext;
        auto* runs = *static_cast<uint32_t**>(ctx.drawList);
        if (!runs) return;
        const uint32_t* run = runs + ctx.drawIndex * off::kShadowRunStride;
        if (!run[off::kShadowRunCountField]) return;
        auto* inst = reinterpret_cast<off::M2Instance*>(run[0]);
        if (!inst || inst->parent || !inst->model) return;
        auto* shared = reinterpret_cast<off::M2Model*>(inst->model);
        const char* path = wxl::game::m2::M2Model(shared).GetPathStem();
        if (!path || (std::strncmp(path,"character\\",10) && std::strncmp(path,"hd\\character\\",13)
                      && std::strncmp(path,"creature\\",9))) return;
        auto* sec = static_cast<fmt::M2SkinSection*>(ctx.section);
        TraceEntry* entry = nullptr;
        for (auto& e : g_traceEntries)
        {
            if (!e.instance) { e.instance=inst; e.shared=shared; e.influences=sec->boneInfluences; entry=&e; break; }
            if (e.instance==inst && e.shared==shared && e.influences==sec->boneInfluences) { entry=&e; break; }
        }
        if (!entry || !entry->gate.Take(GetTickCount())) return;
        const uint32_t id = ++g_traceReports;
        const auto* hdr = static_cast<const fmt::M2Header*>(shared->header);
        uint32_t bone = 0xFFFFFFFFu;
        float selected[16]{};
        bool haveBone = false;
        if (hdr && sec->boneCount && sec->boneComboIndex < hdr->boneCombos.count && hdr->boneCombos.offset)
        {
            bone = reinterpret_cast<const uint16_t*>(hdr->boneCombos.offset)[sec->boneComboIndex];
            if (bone < hdr->bones.count && inst->bonePalettePtr)
            {
                std::memcpy(selected, reinterpret_cast<const float*>(inst->bonePalettePtr) + bone*16, sizeof selected);
                haveBone = true;
            }
        }
        const auto* scene = reinterpret_cast<const off::M2SceneClock*>(inst->scene);
        WLOG_INFO("shadow-space-v1: id=%u tick=%u path='%s' owner=%p batchOwner=%p shared=%p section=%p run=%u count=%u infl=%u bones=%u combo=%u bone=%u validBone=%u frame=%u animated=%u si=%u pc=%u",
            id, GetTickCount(), path, inst, ctx.instance, shared, sec, ctx.drawIndex, run[2],
            sec->boneInfluences, sec->boneCount, sec->boneComboIndex, bone, haveBone?1u:0u,
            scene?scene->frame:0xFFFFFFFFu, inst->lastAnimFrame, startIndex, primitiveCount);
        LogMatrix(id,"placement",inst->placement,4);
        LogMatrix(id,"viewRoot",inst->viewRoot,4);
        if (haveBone) LogMatrix(id,"bone",selected,4);
        // This address is the native float constant lock buffer, NOT the GPU state.
        const auto* cpu = reinterpret_cast<const float*>(off::kVsConstBlock);
        LogMatrix(id,"cpu14",cpu+14*4,3);
        LogMatrix(id,"cpu31",cpu+31*4,3);
        float low[17*4]{}, palette[12]{};
        const HRESULT lowHr=device->GetVertexShaderConstantF(0,low,17);
        const HRESULT palHr=device->GetVertexShaderConstantF(31,palette,3);
        WLOG_INFO("shadow-space-v1: id=%u constantsHr=%08X/%08X cpuBoneError=%.9g gpuBoneError=%.9g",
            id, unsigned(lowHr), unsigned(palHr), haveBone?wxl_modern_m2::shadowtrace::PaletteError(selected,cpu+31*4):-1.f,
            haveBone&&SUCCEEDED(palHr)?wxl_modern_m2::shadowtrace::PaletteError(selected,palette):-1.f);
        if (SUCCEEDED(lowHr)) LogMatrix(id,"gpu0",low,17);
        if (SUCCEEDED(palHr)) LogMatrix(id,"gpu31",palette,3);

        IDirect3DVertexShader9* shader=nullptr;
        const HRESULT shaderHr=device->GetVertexShader(&shader);
        const auto identity=wxl::modern::materialdiag::InspectShader(shader,nullptr,0);
        WLOG_INFO("shadow-space-v1: id=%u shaderHr=%08X vs=%08X bytes=%u read=%s",
            id,unsigned(shaderHr),identity.hash,identity.bytes,wxl::modern::materialdiag::ShaderReadName(identity.state));
        // Small bounded bytecode dump: lets the follow-up inspect the actual variant, rather than
        // assuming that its influence selector implies a particular c14..16 shader program.
        if (shader && identity.state==wxl::modern::materialdiag::ShaderRead::Read && identity.bytes<=4096)
        {
            bool seen=false;
            for (uint32_t i=0;i<g_traceShaderCount;++i) if(g_traceShaders[i]==identity.hash) seen=true;
            if (!seen && g_traceShaderCount<8)
            {
                uint32_t words[1024]{}; UINT size=sizeof words;
                if (SUCCEEDED(shader->GetFunction(words,&size)) && size==identity.bytes && size%4==0)
                {
                    g_traceShaders[g_traceShaderCount++]=identity.hash;
                    for (unsigned i=0;i<size/4;i+=4)
                        WLOG_INFO("shadow-space-v1: vs=%08X word=%u data=%08X,%08X,%08X,%08X",
                            identity.hash,i,words[i],words[i+1],words[i+2],words[i+3]);
                }
            }
        }
        if (shader) shader->Release();
        if (g_traceReports==320) WLOG_INFO("shadow-space-v1: trace budget complete");
    }

    void ProbeShadowDraw(void* instance, void* section)
    {
        auto* inst = static_cast<off::M2Instance*>(instance);
        auto* shared = reinterpret_cast<uint8_t*>(inst->model);
        if (!shared) return;

        auto* sec = static_cast<fmt::M2SkinSection*>(section);

        // --- the value that actually selects the shadow vertex program ---
        const uint16_t inflDraw = sec->boneInfluences;
        const uint32_t ovr = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(inst) + off::kOffInstSectionOverride);

        // --- which array is this section in: the shared runtime's own +0x18C copy, or somewhere else? ---
        auto* copyBase = *reinterpret_cast<uint8_t**>(shared + off::kOffModelSubmeshBuf);
        int32_t secIdx = -1;
        if (copyBase)
        {
            const ptrdiff_t delta = reinterpret_cast<uint8_t*>(sec) - copyBase;
            if (delta >= 0 && (delta % static_cast<ptrdiff_t>(sizeof(fmt::M2SkinSection))) == 0)
                secIdx = static_cast<int32_t>(delta / static_cast<ptrdiff_t>(sizeof(fmt::M2SkinSection)));
        }
        // ...and what the LIVE skin says at that same index, which is what FixSubmeshes patched.
        uint16_t inflSkin = 0xFFFFu;
        auto* skin = static_cast<wxl::game::m2::M2SkinProfile*>(
            reinterpret_cast<off::M2Model*>(shared)->skin);
        if (skin && skin->submeshes && secIdx >= 0 && static_cast<uint32_t>(secIdx) < skin->submeshCount)
            inflSkin = skin->submeshes[secIdx].boneInfluences;

        // --- palette freshness AND space, in one check ---
        const uint32_t lastAnim = inst->lastAnimFrame;
        uint32_t frame = 0xFFFFFFFFu;
        if (auto* scene = reinterpret_cast<off::M2SceneClock*>(inst->scene))
            frame = scene->frame;
        bool palMatchesRoot = true;
        if (const auto* pal = reinterpret_cast<const float*>(inst->bonePalettePtr))
        {
            const float* root = inst->viewRoot;
            palMatchesRoot = (std::fabs(pal[12] - root[12]) + std::fabs(pal[13] - root[13]) +
                              std::fabs(pal[14] - root[14])) < 0.01f;
        }

        if (inflDraw == 0)        g_statInflZero.fetch_add(1, std::memory_order_relaxed);
        if (ovr != 0)             g_statOverride.fetch_add(1, std::memory_order_relaxed);
        if (lastAnim != frame)    g_statStaleAnim.fetch_add(1, std::memory_order_relaxed);
        if (!palMatchesRoot)      g_statPaletteMismatch.fetch_add(1, std::memory_order_relaxed);

        // --- the intervention, folded into the same pass ---
        // A zero here means this draw takes the shadow variant that never applies c14..c16. The section
        // is a runtime copy, so lifting it to 1 is exactly what FixSubmeshes was supposed to guarantee;
        // doing it at the draw closes every path that could have bypassed it. Never touches the file.
        if (inflDraw == 0 && sec->indexCount != 0 && g_enabled.load(std::memory_order_relaxed))
        {
            sec->boneInfluences = 1;
            const uint32_t n = g_statInflFixed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8)
            {
                const char* stem = wxl::game::m2::M2Model(shared).GetPathStem();
                WLOG_WARN("m2shadow: lifted boneInfluences 0 -> 1 at the shadow draw for '%s' sec=%d "
                          "(inflSkin=%u ovr=0x%X) -- this section would have been camera-locked",
                          stem ? stem : "(no stem)", secIdx, inflSkin, ovr);
            }
        }
    }
}

namespace wxl::runtime::m2shadow
{
    TraceScope::TraceScope(void* instance, void* section, void* drawList, uint32_t drawIndex) noexcept
        : context_{instance,section,drawList,drawIndex,false}, previous_(g_traceContext)
    {
        if (g_traceEnabled || g_indexMode) g_traceContext=&context_;
    }
    TraceScope::~TraceScope() { g_traceContext=previous_; }

    bool HasShadowContext() noexcept { return g_traceContext!=nullptr; }

    unsigned PrepareDIP(void* device, int primitiveType, unsigned startIndex, unsigned primitiveCount) noexcept
    {
        if (!g_indexMode || !g_traceContext || !device) return startIndex;
        IndexCandidate candidate{};
        __try
        {
            if (!ReadIndexCandidate(primitiveType,startIndex,primitiveCount,candidate)) return startIndex;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (g_indexFaults++<8) WLOG_WARN("shadow-index-v1: candidate read failed; native draw unchanged");
            return startIndex;
        }
        uint32_t bytes=0; HRESULT hr=E_FAIL;
        const bool fits=BoundIndexBufferFits(static_cast<IDirect3DDevice9*>(device),candidate,bytes,hr);
        const bool applied=fits && g_indexMode==2;
        if (g_indexReports++<48)
            WLOG_INFO("shadow-index-v1: mode=%u applied=%u path='%s' instance=%p start=%u full=%u count=%u skinIndices=%u ibBytes=%u ibHr=%08X fits=%u",
                g_indexMode,applied?1u:0u,candidate.path,candidate.instance,startIndex,candidate.full,
                candidate.input.sectionCount,candidate.input.skinIndices,bytes,unsigned(hr),fits?1u:0u);
        return applied?candidate.full:startIndex;
    }

    void BeforeDIP(void* device, unsigned startIndex, unsigned primitiveCount) noexcept
    {
        if (!g_traceEnabled || !g_traceContext || g_traceContext->sawDraw) return;
        g_traceContext->sawDraw=true;
        if (!device || !g_traceContext->section || !g_traceContext->drawList || g_traceReports>=320) return;
        __try { TraceDraw(static_cast<IDirect3DDevice9*>(device),startIndex,primitiveCount); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Also bound failing samples: malformed native data must not produce a log storm.
            ++g_traceReports;
            WLOG_WARN("shadow-space-v1: native snapshot fault; draw untouched; reports=%u",g_traceReports);
        }
    }

    void Arm(bool hookInstalled)
    {
        g_traceEnabled = wxl_modern_m2::ConfigU32("WXL_M2_SHADOW_SPACE_TRACE",0,0,1)!=0;
        g_indexMode = wxl_modern_m2::ConfigU32("WXL_M2_SHADOW_INDEX_FIX",wxl_modern_m2::shadowindex::kDefaultMode,0,2);
        WLOG_INFO("shadow-index-v1: mode=%u (0=off 1=observe 2=repair; singleton modern extended sections only)",g_indexMode);
        WLOG_INFO("shadow-space-v1: trace=%u (read-only; first DIP per native run; max 320 samples)",g_traceEnabled?1u:0u);
        g_armed = hookInstalled;
        WLOG_INFO("m2shadow: shadow-draw probe + boneInfluences guard %s (rides the existing 0x%08X detour)",
                  hookInstalled ? "armed" : "NOT armed -- host hook missing",
                  static_cast<unsigned>(off::kRenderBatchShadowMap));
    }

    void OnShadowBatch(void* instance, void* section)
    {
        g_statDraws.fetch_add(1, std::memory_order_relaxed);
        if (!instance || !section) return;
        __try { ProbeShadowDraw(instance, section); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_statFaults.fetch_add(1, std::memory_order_relaxed); }
    }

    bool Installed() { return g_armed; }
    bool Enabled()   { return g_enabled.load(std::memory_order_relaxed); }

    void SetEnabled(bool on)
    {
        // Log only on an actual transition: the Lua panel writes the checkbox back every frame.
        if (g_enabled.exchange(on, std::memory_order_relaxed) != on)
            WLOG_INFO("m2shadow: boneInfluences guard %s", on ? "ENABLED" : "disabled (stock)");
    }

    Stats GetStats()
    {
        Stats s{};
        s.shadowDraws      = g_statDraws.load(std::memory_order_relaxed);
        s.influencesZero   = g_statInflZero.load(std::memory_order_relaxed);
        s.influencesFixed  = g_statInflFixed.load(std::memory_order_relaxed);
        s.overrideSections = g_statOverride.load(std::memory_order_relaxed);
        s.staleAnim        = g_statStaleAnim.load(std::memory_order_relaxed);
        s.paletteMismatch  = g_statPaletteMismatch.load(std::memory_order_relaxed);
        s.faults           = g_statFaults.load(std::memory_order_relaxed);
        return s;
    }
}
