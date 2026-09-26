// Native world-model consumer for the allowlisted retail spell visual bridge.
// DB2 graph parsing remains owned by wxl-db2; this file only resolves and renders M2 models.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"
#include "render/RiftDiagnosticPolicy.hpp"
#include "RiftPlaybackPolicy.hpp"
#include "compat/ModernM2.hpp"

#include "engine/events/Event.hpp"
#include "game/M2.hpp"
#include "game/Script.hpp"
#include "game/Unit.hpp"
#include "game/World.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace gm2 = wxl::game::m2;
    namespace m2off = wxl::offsets::game::m2;
    namespace script = wxl::game::script;
    namespace unit = wxl::game::unit;
    namespace world = wxl::game::world;

    constexpr uint32_t kEntropicRiftVisualSpell = 447445;
    constexpr uint32_t kRootVisualSlot = 19;
    constexpr uint32_t kRetryMs = 250;
    constexpr float kEntropicRiftCompatibilityScale = 3.0f;
    namespace rd=wxl::modern::riftdiag;
    namespace rp=wxl::modern::riftplayback;
    bool g_riftPlayback=false;
    unsigned g_lifecycleReports=0;
    bool ReportLifecycle() {return rd::enabled.load() && g_lifecycleReports++<64;}
    int __cdecl ObserveRift(const char* name,const uint8_t* raw,uint32_t size,const WXL_ByteSink*) {
        static std::atomic<unsigned> reports{0};
        if(!rd::enabled.load() || !name || !rd::Target(name) || !raw || size<8 || size>8u*1024*1024 || reports.fetch_add(1)>=4)return 0;
        WLOG_INFO("rift-diag-v1: source path='%s' bytes=%u fnv64=%016llx; observation only",name,size,rd::Fingerprint({raw,size}));
        uint32_t at=0,n=0;
        while(at<=size && size-at>=8 && n++<24) {
            uint32_t length;std::memcpy(&length,raw+at+4,4);
            if(length>size-at-8){WLOG_WARN("rift-diag-v1: source chunk range invalid");break;}
            char tag[5]{};std::memcpy(tag,raw+at,4);
            WLOG_INFO("rift-diag-v1: chunk tag=%.4s bytes=%u",tag,length);
            at+=8+length;
        }
        return 0; // Never write to sink or replace bytes.
    }

    enum class CatalogState : uint8_t { NotStarted, Loading, Ready, Failed };
    struct ModelChoice { uint32_t fdid = 0; std::string path; float scale = 1.0f; };
    struct PendingVisual
    {
        uint32_t spellId = 0;
        uint64_t guid = 0;
        uint32_t expiresAt = 0;
        uint32_t nextAttemptAt = 0;
    };
    struct ActiveVisual
    {
        uint32_t spellId = 0;
        uint64_t guid = 0;
        void* root = nullptr;
        void* renderContext = nullptr;
        uint32_t slot = 0;
        float scale = 1.0f;
        uint32_t expiresAt = 0;
        rp::State playback;
        uint32_t playbackRetryAt=0, playbackReportAt=0;
        unsigned playbackReports=0,playbackWaitReports=0;
    };

    std::atomic<CatalogState> g_catalogState{CatalogState::NotStarted};
    std::mutex g_catalogMutex;
    std::vector<ModelChoice> g_modelChoices;
    std::string g_catalogError;
    std::vector<PendingVisual> g_pending;
    std::vector<ActiveVisual> g_active;
    bool g_modelsPrewarmed = false;
    bool g_scriptRegistered = false;
    uint32_t g_nextPrewarmAttemptAt = 0;

    bool TickReached(uint32_t now, uint32_t deadline) noexcept
    {
        return static_cast<int32_t>(now - deadline) >= 0;
    }

    void SetCatalogFailure(std::string error)
    {
        {
            std::lock_guard lock(g_catalogMutex);
            g_catalogError = std::move(error);
            g_modelChoices.clear();
        }
        g_catalogState.store(CatalogState::Failed, std::memory_order_release);
        WLOG_WARN("retail-spell-visual: %s", g_catalogError.c_str());
    }

    DWORD WINAPI BuildCatalog(LPVOID)
    {
        const WXL_RetailSpellDb2Api* graph = wxl_modern_m2::RetailSpellDb2();
        const WXL_FdidApi* fdid = wxl_modern_m2::Fdid();
        if (!graph || !graph->Enabled || !graph->Enabled())
        {
            SetCatalogFailure("wxl-db2 retail spell service is disabled");
            return 0;
        }
        if (!fdid || !fdid->ResolveModel)
        {
            SetCatalogFailure("wxl-db2 model FileDataID resolver is unavailable");
            return 0;
        }

        void* lease = graph->Acquire(kEntropicRiftVisualSpell);
        if (!lease)
        {
            SetCatalogFailure("retail spell graph could not be acquired");
            return 0;
        }
        struct Release
        {
            const WXL_RetailSpellDb2Api* api;
            void* lease;
            ~Release() { api->Release(lease); }
        } release{graph, lease};

        std::vector<ModelChoice> choices;
        const uint32_t count = graph->ModelCount(lease);
        choices.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            WXL_RetailSpellModel model{};
            if (!graph->ModelAt(lease, i, &model) || !model.fileDataId) continue;
            const char* resolved = fdid->ResolveModel(model.fileDataId);
            if (!resolved || !*resolved) continue;
            std::string path(resolved);
            std::replace(path.begin(), path.end(), '/', '\\');
            float scale = model.scale;
            if (!std::isfinite(scale) || scale <= 0.0f) scale = 1.0f;
            choices.push_back(ModelChoice{
                model.fileDataId, std::move(path),
                scale * kEntropicRiftCompatibilityScale});
        }

        if (choices.empty())
        {
            const char* error = graph->Error ? graph->Error(lease) : nullptr;
            SetCatalogFailure(error && *error ? error : "retail model FileDataIDs did not resolve");
            return 0;
        }

        {
            std::lock_guard lock(g_catalogMutex);
            g_modelChoices = std::move(choices);
            g_catalogError.clear();
        }
        g_catalogState.store(CatalogState::Ready, std::memory_order_release);
        WLOG_INFO("retail-spell-visual: ready spell=%u models=%zu",
                  kEntropicRiftVisualSpell, g_modelChoices.size());
        return 0;
    }

    bool StartCatalog()
    {
        CatalogState expected = CatalogState::NotStarted;
        if (!g_catalogState.compare_exchange_strong(
                expected, CatalogState::Loading, std::memory_order_acq_rel))
            return expected == CatalogState::Loading || expected == CatalogState::Ready;

        HANDLE thread = CreateThread(nullptr, 0, &BuildCatalog, nullptr, 0, nullptr);
        if (!thread)
        {
            SetCatalogFailure("failed to create catalog worker");
            return false;
        }
        CloseHandle(thread);
        return true;
    }

    void* SafeReadPointer(void* base, size_t offset) noexcept
    {
        if (!base) return nullptr;
        __try
        {
            return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void SafeDetach(const ActiveVisual& visual) noexcept
    {
        void* object = world::ResolveObject(visual.guid, world::kTypeMaskUnit);
        if (!object || unit::Model(object) != visual.root) return;
        __try { gm2::DetachSlot(visual.root, visual.slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool SafeCreateAndAttach(void* owner, void* root, char* path,
                             uint32_t slot, void*& renderContext) noexcept
    {
        renderContext = nullptr;
        __try
        {
            renderContext = gm2::GetRenderCtx(owner, path);
            if (!renderContext) return false;
            gm2::AttachToScene(renderContext, root, slot, true);
            const bool attached = SafeReadPointer(renderContext, m2off::kOffInstParent) == root;
            gm2::ReleaseRenderCtx(renderContext);
            return attached;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafePrewarmModel(void* owner, char* path) noexcept
    {
        __try
        {
            void* renderContext = gm2::GetRenderCtx(owner, path);
            if (!renderContext) return false;
            gm2::ReleaseRenderCtx(renderContext);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void TryPrewarmModels(uint32_t now)
    {
        if (g_modelsPrewarmed || !TickReached(now, g_nextPrewarmAttemptAt)) return;
        const uint64_t guid = world::ActivePlayerGuid();
        void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        void* root = player ? unit::Model(player) : nullptr;
        void* owner = SafeReadPointer(root, m2off::kOffSceneNodeOwner);
        if (!root || !owner)
        {
            g_nextPrewarmAttemptAt = now + kRetryMs;
            return;
        }

        std::vector<ModelChoice> choices;
        { std::lock_guard lock(g_catalogMutex); choices = g_modelChoices; }
        if (choices.empty()) return;
        for (const ModelChoice& choice : choices)
        {
            if (!SafePrewarmModel(owner, const_cast<char*>(choice.path.c_str())))
            {
                g_nextPrewarmAttemptAt = now + kRetryMs;
                return;
            }
        }
        g_modelsPrewarmed = true;
        WLOG_INFO("retail-spell-visual: prewarmed models=%zu", choices.size());
    }

    void SafeApplyPlacement(void* renderContext, float scale) noexcept
    {
        if (!renderContext || !std::isfinite(scale) || scale <= 0.0f) return;
        __try
        {
            float* placement = reinterpret_cast<float*>(
                reinterpret_cast<uint8_t*>(renderContext) + m2off::kOffInstPlacement);
            for (int basis = 0; basis < 3; ++basis)
            {
                const int start = basis * 4;
                const float x = placement[start];
                const float y = placement[start + 1];
                const float z = placement[start + 2];
                const float length = std::sqrt(x * x + y * y + z * z);
                if (!std::isfinite(length) || length <= 0.0001f) continue;
                const float factor = scale / length;
                placement[start] *= factor;
                placement[start + 1] *= factor;
                placement[start + 2] *= factor;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void StopVisual(uint32_t spellId, uint64_t guid)
    {
        const size_t beforePending=g_pending.size(),beforeActive=g_active.size();
        std::erase_if(g_pending, [&](const PendingVisual& visual) {
            return visual.spellId == spellId && visual.guid == guid;
        });
        for (size_t i = 0; i < g_active.size();)
        {
            if (g_active[i].spellId != spellId || g_active[i].guid != guid)
            {
                ++i;
                continue;
            }
            SafeDetach(g_active[i]);
            g_active.erase(g_active.begin() + static_cast<ptrdiff_t>(i));
        }
        if(ReportLifecycle())WLOG_INFO("rift-diag-v1: stop guid=%016llx removedPending=%zu removedActive=%zu; detach attempted, not visible-cleanup proof",guid,beforePending-g_pending.size(),beforeActive-g_active.size());
    }

    struct PlaybackSnapshot {
        void* shared=nullptr;
        const wxl::structure::m2::M2Header* header=nullptr;
        uint32_t pending=UINT32_MAX;
        uint16_t index=UINT16_MAX,assigned=UINT16_MAX;
        int32_t start=0,end=0,timeOffset=0;
    };
    bool SafePlaybackSnapshot(const ActiveVisual& visual,PlaybackSnapshot& out) noexcept
    {
        __try {
            auto* instance=static_cast<m2off::M2Instance*>(visual.renderContext);
            if(!instance || SafeReadPointer(instance,m2off::kOffInstParent)!=visual.root || !instance->model || !instance->boneStates)return false;
            out.shared=reinterpret_cast<void*>(instance->model);
            gm2::M2Model model(out.shared);
            const char* path=model.GetPathStem();
            if(!path)return false;
            char bounded[276]{};size_t n=0;
            for(;n+1<sizeof(bounded)&&path[n];++n)bounded[n]=path[n];
            if(path[n] || !rd::Target(bounded))return false;
            out.header=model.GetHeader();
            if(!out.header || !out.header->bones.count || out.header->sequences.count!=4 || !out.header->sequences.offset)return false;
            if(!rp::SequenceContract({reinterpret_cast<const wxl::structure::m2::M2Sequence*>(out.header->sequences.offset),4}))return false;
            const auto* bone=reinterpret_cast<const m2off::RuntimeBone*>(instance->boneStates);
            out.index=bone->animIndex;out.assigned=bone->assignedSeq;
            out.pending=bone->pendingSeq;out.start=bone->seqStart;out.end=bone->seqEnd;out.timeOffset=bone->timeOffset;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool SafePlayPhase(void* instance,uint32_t id) noexcept
    {
        __try {
            wxl::game::Native<m2off::M2_SetBoneSequenceFn>(m2off::kSetBoneSequence)(
                instance,nullptr,UINT32_MAX,id,UINT32_MAX,0,1.0f,1,1);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void UpdatePlayback(ActiveVisual& visual,uint32_t now)
    {
        if(!g_riftPlayback || visual.playback.phase==rp::Phase::Disabled)return;
        if(visual.playback.phase==rp::Phase::Waiting && visual.playbackRetryAt && !TickReached(now,visual.playbackRetryAt))return;
        const bool reportDue=visual.playbackReports<12 && (!visual.playbackReports || TickReached(now,visual.playbackReportAt));
        if(rp::Request(visual.playback,now)==rp::kNone && !reportDue)return;
        PlaybackSnapshot snapshot{};
        if(!SafePlaybackSnapshot(visual,snapshot)){
            visual.playbackRetryAt=now+kRetryMs;
            if(visual.playbackWaitReports++<4)WLOG_INFO("rift-playback-v1: waiting instance=%p reason=readiness-parent-or-sequence-contract; no request issued",visual.renderContext);
            return; // asynchronous readiness: never reset the lifetime or dereference missing arrays
        }
        if(!wxl::modern::assets::m2::IsNativeLoaded(snapshot.shared))return;
        auto source=wxl::modern::assets::m2::material::SourceMaterials().FindModel(snapshot.header);
        if(!source || source->riftNativeSpriteMask!=0x21f){
            if(visual.playbackWaitReports++<4)WLOG_INFO("rift-playback-v1: waiting instance=%p reason=source-identity; no request issued",visual.renderContext);
            visual.playbackRetryAt=now+kRetryMs;return; // original full-file identity contract
        }
        const uint32_t request=rp::Request(visual.playback,now);
        if(request!=rp::kNone){
            const bool called=SafePlayPhase(visual.renderContext,request);
            if(!called || !rp::Commit(visual.playback,request,now)){
                visual.playback.phase=rp::Phase::Disabled;
                WLOG_WARN("rift-playback-v1: disabled instance=%p request=%u; native call failed, original lifetime retained",visual.renderContext,request);
                return;
            }
            WLOG_INFO("rift-playback-v1: request instance=%p sequence=%u elapsed=%u expires=%u; request is not playback proof",visual.renderContext,request,now-visual.playback.startedAt,visual.expiresAt);
        }
        if(reportDue){
            ++visual.playbackReports;visual.playbackReportAt=now+500;
            // Pre-call snapshot intentionally distinguishes pending requests from evaluated state.
            WLOG_INFO("rift-playback-v1: sample instance=%p phase=%u elapsed=%u index=%u assigned=%u pending=%u start=%d end=%d offset=%d; root-bone sample, not all-track proof",visual.renderContext,unsigned(visual.playback.phase),now-visual.playback.startedAt,unsigned(snapshot.index),unsigned(snapshot.assigned),snapshot.pending,snapshot.start,snapshot.end,snapshot.timeOffset);
        }
    }

    bool TryActivate(const PendingVisual& pending)
    {
        std::vector<ModelChoice> choices;
        { std::lock_guard lock(g_catalogMutex); choices = g_modelChoices; }
        if (choices.empty()) return false;

        void* object = world::ResolveObject(pending.guid, world::kTypeMaskUnit);
        void* root = object ? unit::Model(object) : nullptr;
        void* owner = SafeReadPointer(root, m2off::kOffSceneNodeOwner);
        if (!root || !owner) return false;

        const size_t start = g_active.size();
        for (size_t i = 0; i < choices.size(); ++i)
        {
            void* renderContext = nullptr;
            const ModelChoice& choice = choices[i];
            if (!SafeCreateAndAttach(owner, root, const_cast<char*>(choice.path.c_str()),
                                     kRootVisualSlot + static_cast<uint32_t>(i), renderContext))
            {
                while (g_active.size() > start)
                {
                    SafeDetach(g_active.back());
                    g_active.pop_back();
                }
                return false;
            }
            g_active.push_back(ActiveVisual{
                pending.spellId, pending.guid, root, renderContext,
                kRootVisualSlot + static_cast<uint32_t>(i), choice.scale, pending.expiresAt});
            if(ReportLifecycle())WLOG_INFO("rift-diag-v1: attached guid=%016llx path='%s' fdid=%u slot=%u scale=%.3f expires=%u",pending.guid,choice.path.c_str(),choice.fdid,kRootVisualSlot+static_cast<uint32_t>(i),choice.scale,pending.expiresAt);
        }
        return true;
    }

    uint32_t LuaUnsigned(void* state, int index) noexcept
    {
        if (!state || script::ArgCount(state) < index || !script::IsNumber(state, index)) return 0;
        const double value = script::ToNumber(state, index);
        if (!std::isfinite(value) || value < 0.0 ||
            value > static_cast<double>((std::numeric_limits<uint32_t>::max)())) return 0;
        return static_cast<uint32_t>(value);
    }

    int __cdecl LuaSpawn(void* state)
    {
        const uint32_t spellId = LuaUnsigned(state, 1);
        const uint32_t high = LuaUnsigned(state, 2);
        const uint32_t low = LuaUnsigned(state, 3);
        const uint32_t duration = (std::max)(250u, LuaUnsigned(state, 4));
        if (spellId != kEntropicRiftVisualSpell || (!high && !low)) return 0;
        const uint64_t guid = (static_cast<uint64_t>(high) << 32) | low;
        StopVisual(spellId, guid);
        const uint32_t now = GetTickCount();
        g_pending.push_back(PendingVisual{spellId, guid, now + duration, now});
        if(ReportLifecycle())WLOG_INFO("rift-diag-v1: spawn guid=%016llx duration=%u expires=%u",guid,duration,now+duration);
        StartCatalog();
        return 0;
    }

    int __cdecl LuaStop(void* state)
    {
        const uint32_t spellId = LuaUnsigned(state, 1);
        const uint32_t high = LuaUnsigned(state, 2);
        const uint32_t low = LuaUnsigned(state, 3);
        if (spellId == kEntropicRiftVisualSpell && (high || low))
            StopVisual(spellId, (static_cast<uint64_t>(high) << 32) | low);
        return 0;
    }

    constexpr char kBootstrap[] = R"lua(
do
    wxlwow = wxlwow or {}
    wxlwow.play_retail_spell_visual = _WXLWOW_PLAY_RETAIL_SPELL_VISUAL
    wxlwow.stop_retail_spell_visual = _WXLWOW_STOP_RETAIL_SPELL_VISUAL
    _WXLWOW_PLAY_RETAIL_SPELL_VISUAL = nil
    _WXLWOW_STOP_RETAIL_SPELL_VISUAL = nil

    local owner = _G.WXLRetailSpellVisuals or {}
    _G.WXLRetailSpellVisuals = owner
    if not owner.frame then
        owner.frame = CreateFrame("Frame")
        owner.frame:RegisterEvent("CHAT_MSG_ADDON")
        owner.frame:SetScript("OnEvent", function(_, _, prefix, message)
            if prefix ~= "WXL_RETAIL" or type(message) ~= "string" then return end
            local spellId, high, low, duration =
                string.match(message, "^SPAWN\t(%d+)\t(%d+)\t(%d+)\t(%d+)$")
            if spellId then
                wxlwow.play_retail_spell_visual(
                    tonumber(spellId), tonumber(high), tonumber(low), tonumber(duration))
                return
            end
            spellId, high, low = string.match(message, "^STOP\t(%d+)\t(%d+)\t(%d+)$")
            if spellId then
                wxlwow.stop_retail_spell_visual(tonumber(spellId), tonumber(high), tonumber(low))
            end
        end)
    end
end
)lua";

    void TryRegisterScript()
    {
        if (g_scriptRegistered) return;
        const WXL_FrameScriptApi* api = wxl_modern_m2::FrameScript();
        if (!api) return; // wxl-runtime loads alphabetically after wxl-modern-m2; retry on first frame.
        g_scriptRegistered =
            api->RegisterFunction("_WXLWOW_PLAY_RETAIL_SPELL_VISUAL", &LuaSpawn) &&
            api->RegisterFunction("_WXLWOW_STOP_RETAIL_SPELL_VISUAL", &LuaStop) &&
            api->RegisterScript("retail-spell-visual", kBootstrap);
        if (g_scriptRegistered)
            WLOG_INFO("retail-spell-visual: FrameScript bridge registered");
    }

    void __cdecl OnUpdate(void*, const void*)
    {
        TryRegisterScript();
        const uint32_t now = GetTickCount();
        for (size_t i = 0; i < g_active.size();)
        {
            ActiveVisual& visual = g_active[i];
            void* object = world::ResolveObject(visual.guid, world::kTypeMaskUnit);
            if (!object || unit::Model(object) != visual.root || TickReached(now, visual.expiresAt))
            {
                if(ReportLifecycle())WLOG_INFO("rift-diag-v1: retire guid=%016llx expired=%u ownerPresent=%u",visual.guid,unsigned(TickReached(now,visual.expiresAt)),unsigned(object!=nullptr));
                if (object && unit::Model(object) == visual.root) SafeDetach(visual);
                g_active.erase(g_active.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            UpdatePlayback(visual,now);
            ++i;
        }

        if (g_catalogState.load(std::memory_order_acquire) != CatalogState::Ready) return;
        TryPrewarmModels(now);
        for (size_t i = 0; i < g_pending.size();)
        {
            PendingVisual& pending = g_pending[i];
            if (TickReached(now, pending.expiresAt))
            {
                if(ReportLifecycle())WLOG_WARN("rift-diag-v1: pending expired guid=%016llx without attachment",pending.guid);
                g_pending.erase(g_pending.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            if (!TickReached(now, pending.nextAttemptAt)) { ++i; continue; }
            pending.nextAttemptAt = now + kRetryMs;
            if (TryActivate(pending))
            {
                g_pending.erase(g_pending.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    void __cdecl OnM2PerFrame(void*, const void* raw)
    {
        const auto& args = *static_cast<const ev::M2PerFrameUpdateArgs*>(raw);
        for (const ActiveVisual& visual : g_active)
            if (visual.renderContext == args.renderCtx)
                SafeApplyPlacement(visual.renderContext, visual.scale);
    }

    void __cdecl OnWorldLeave(void*, const void*)
    {
        if(ReportLifecycle())WLOG_INFO("rift-diag-v1: world-leave pending=%zu active=%zu; bookkeeping clear",g_pending.size(),g_active.size());
        g_pending.clear();
        g_active.clear();
        g_modelsPrewarmed = false;
        g_nextPrewarmAttemptAt = 0;
    }
}

bool wxl_modern_m2::InstallRetailSpellVisuals()
{
    g_riftPlayback=ConfigBool("WXL_M2_RIFT_PLAYBACK",false);
    if(g_riftPlayback)WLOG_INFO("rift-playback-v1: enabled exact-source startup213->hold158; no clock freeze, lifetime/Stop unchanged");
    rd::enabled.store(ConfigBool("WXL_M2_RIFT_DIAGNOSTICS",false));
    if(rd::enabled.load()) {
        const auto* storage=Storage();
        if(storage && storage->structSize>=sizeof(WXL_StorageApi) && storage->RegisterClientTransform) {
            storage->RegisterClientTransform("cfx_priest_entropicrift_areatrigger.m2",&ObserveRift);
            storage->RegisterClientTransform("cfx_priest_entropicrift_areatrigger.mdx",&ObserveRift);
        }
        WLOG_INFO("rift-diag-v1: enabled source/admission/lifecycle; no renderer overrides");
    }
    const WXL_RetailSpellDb2Api* graph = RetailSpellDb2();
    if (!graph || !graph->Enabled || !graph->Enabled())
    {
        WLOG_INFO("retail-spell-visual: DB2 graph is disabled; renderer remains inactive");
        return true;
    }

    g_api->Subscribe(uint32_t(ev::Event::OnUpdate), &OnUpdate, nullptr);
    g_api->Subscribe(uint32_t(ev::Event::OnM2PerFrameUpdate), &OnM2PerFrame, nullptr);
    g_api->Subscribe(uint32_t(ev::Event::OnWorldLeave), &OnWorldLeave, nullptr);
    StartCatalog();
    WLOG_INFO("retail-spell-visual: native M2 consumer registered");
    return true;
}
