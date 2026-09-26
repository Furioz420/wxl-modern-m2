// Guarded native presentation projection + capability/impact evidence. GPLv3.
#include "ExtensionApi.hpp"
#include "client/Spell/LegacyPresentation.hpp"
#include "client/Spell/PresentationAnimation.hpp"
#include "game/Io.hpp"
#include "game/M2.hpp"
#include "game/M2Animation.hpp"
#include "game/Unit.hpp"
#include "game/World.hpp"
#include <atomic>
#include <string>

namespace
{
    namespace p=wxl::spell::presentation;
    namespace io=wxl::game::io;
    namespace ev=wxl::events;
    std::atomic<unsigned> g_reports{0}, g_impactReads{0};
    unsigned g_capabilityReports=0;
    uint32_t g_nextCapability=0;
    void* g_lastModel=nullptr;
    unsigned g_lastMask=~0u;
    bool g_poseTest=false;
    std::string Normalize(const char* name);
    bool PoseMetadataReady() noexcept
    {
        uint32_t checkingId=0;
        __try
        {
            for(size_t i=0;i<2;++i)
            {
                checkingId=p::kPoseIds[i];
                const auto* r=wxl::game::m2animation::Lookup(checkingId);
                if(!r)
                {
                    WLOG_WARN("spell-animation-v2: metadata id=%u missing; native poses retained",checkingId);
                    return false;
                }
                WLOG_INFO("spell-animation-v2: metadata id=%u actual=%u fallback=%u behavior=%u flags=%u weapon=%u body=%u tier=%u",checkingId,r->id,r->fallback,r->behaviorId,r->flags,r->weaponFlags,r->bodyFlags,r->behaviorTier);
                if(!r || !r->name || std::strcmp(r->name,p::kPoseNames[i]) || r->id!=p::kPoseIds[i] ||
                   r->fallback!=p::kPoseFallbacks[i] || r->behaviorId!=p::kPoseFallbacks[i] || r->flags!=2 ||
                   r->weaponFlags!=4 || r->bodyFlags!=(i?264u:256u) || r->behaviorTier!=0)
                {
                    WLOG_WARN("spell-animation-v2: metadata id=%u identity/classification mismatch; native poses retained",checkingId);
                    return false;
                }
            }
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("spell-animation-v2: metadata id=%u access fault; native poses retained",checkingId);
            return false;
        }
    }
    int __cdecl TransformAnimationData(const char* name,const uint8_t* raw,uint32_t size,const WXL_ByteSink* sink)
    {
        if(!raw || !sink || !sink->Write || Normalize(name)!="dbfilesclient\\animationdata.dbc")return 0;
        try
        {
            std::vector<uint8_t> output;
            const bool ok=p::ProjectAnimationData({raw,size},output);
            WLOG_INFO("spell-animation-v1: metadata projection=%u ids=922,926 fallback=51,53",unsigned(ok));
            if(!ok)return 0;
            sink->Write(sink->ctx,output.data(),static_cast<uint32_t>(output.size()));return 1;
        }
        catch(...){return 0;}
    }
    std::string Normalize(const char* name)
    {
        std::string s=name ? name : "";
        for (char& c:s) { if(c=='/') c='\\'; if(c>='A' && c<='Z') c+=32; }
        return s;
    }
    bool ReadAndVerify(const char* path, bool (*verify)(p::Bytes) noexcept)
    {
        void* handle=nullptr;
        if (!io::FileOpen(path,0,&handle) || !handle) return false;
        struct Close { void* h; ~Close(){io::FileClose(h);} } close{handle};
        uint32_t high=0, got=0;
        const uint32_t size=io::FileSize(handle,&high);
        if(high || size<20 || size>80u*1024*1024) return false;
        std::vector<uint8_t> bytes(size);
        return io::FileRead(handle,bytes.data(),size,&got) && got==size && verify(bytes);
    }
    int __cdecl TransformKit(const char* name, const uint8_t* raw, uint32_t size, const WXL_ByteSink* sink)
    {
        if (!raw || !sink || !sink->Write || Normalize(name)!="dbfilesclient\\spellvisualkit.dbc") return 0;
        try
        {
            std::vector<uint8_t> output;
            // Validate the actual archive-resolution results, not a guessed locale or loose folder.
            // Nested reads cannot recurse: only SpellVisualKit.dbc is registered for projection.
            const bool ok=p::ProjectRelease(p::Bytes(raw,size),output) &&
                ReadAndVerify("DBFilesClient\\Spell.dbc",&p::VerifySpells) &&
                ReadAndVerify("DBFilesClient\\SpellVisual.dbc",&p::VerifyVisuals) &&
                ReadAndVerify("DBFilesClient\\SpellVisualKitModelAttach.dbc",&p::VerifyAttachments) &&
                ReadAndVerify("DBFilesClient\\SpellVisualEffectName.dbc",&p::VerifyEffects);
            if (g_reports.fetch_add(1)<8)
                WLOG_INFO("spell-presentation-v1: release projection=%u legacy=50796,59170,59171,59172 visual=23335 kit=23462 donor=116858/74214 hands=12630 nativeAnimation=53; gameplay unchanged",unsigned(ok));
            if(!ok) return 0;
            if(g_poseTest)
            {
                // Preserve short-circuiting; -1 explicitly means skipped, not a failed check.
                const bool metadata=PoseMetadataReady();
                const int visual=metadata ? int(ReadAndVerify("DBFilesClient\\SpellVisual.dbc",&p::VerifyPoseVisuals)) : -1;
                const int attachments=visual==1 ? int(ReadAndVerify("DBFilesClient\\SpellVisualKitModelAttach.dbc",&p::VerifyPoseAttachments)) : -1;
                const int kit=attachments==1 ? int(p::ProjectPoseKits({raw,size},output)) : -1;
                const bool poses=kit==1;
                WLOG_INFO("spell-animation-v2: gates metadata=%u visual=%d attachments=%d kit=%d (-1=skipped)",unsigned(metadata),visual,attachments,kit);
                WLOG_INFO("spell-animation-v1: kit projection=%u precast=922 release=926 start=unchanged; stock fallback=51,53",unsigned(poses));
                p::animationProjectionActive.store(poses);
            }
            sink->Write(sink->ctx,output.data(),static_cast<uint32_t>(output.size()));
            return 1;
        }
        catch (...) { WLOG_WARN("spell-presentation-v1: projection exception; native bytes retained"); return 0; }
    }
    int __cdecl ObserveImpact(const char* name, const uint8_t* raw, uint32_t size, const WXL_ByteSink*)
    {
        if (raw && size>=4 && g_impactReads.fetch_add(1)<8)
            WLOG_INFO("spell-presentation-v1: impact archive-read path='%s' bytes=%u magic=%08X; observation only",name,size,p::Word(p::Bytes(raw,size),0));
        return 0; // Do not change the bytes or claim that a read proves a draw.
    }
    void __cdecl OnUpdate(void*, const void*)
    {
        const uint32_t now=GetTickCount();
        if (g_capabilityReports>=8 || static_cast<int32_t>(now-g_nextCapability)<0) return;
        g_nextCapability=now+1000;
        // Only the active WORLD character, never a glue preview. These queries do not play an animation.
        __try
        {
            namespace world=wxl::game::world;
            const uint64_t guid=world::ActivePlayerGuid();
            void* player=guid ? world::ResolveObject(guid,world::kTypeMaskPlayer) : nullptr;
            void* model=player ? wxl::game::unit::Model(player) : nullptr;
            if(!model || !wxl::game::m2animation::ModelData(model)) return;
            unsigned mask=0;
            constexpr uint32_t ids[]{920,922,926};
            for(unsigned i=0;i<3;++i) if(wxl::game::m2animation::ModelHasSequence(model,ids[i])) mask|=1u<<i;
            if(model==g_lastModel && mask==g_lastMask) return;
            g_lastModel=model; g_lastMask=mask; ++g_capabilityReports;
            auto* shared=reinterpret_cast<void*>(reinterpret_cast<wxl::offsets::game::m2::M2Instance*>(model)->model);
            const char* stem=shared ? wxl::game::m2::M2Model(shared).GetPathStem() : nullptr;
            WLOG_INFO("spell-presentation-v1: world-character model=%p path='%s' sequence920=%u sequence922=%u sequence926=%u; availability only; poseProjection=%u",model,stem?stem:"?",mask&1,(mask>>1)&1,(mask>>2)&1,unsigned(p::animationProjectionActive.load()));
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            ++g_capabilityReports;
            WLOG_WARN("spell-presentation-v1: world-character capability unavailable; native animations retained");
        }
    }
}
bool wxl_modern_m2::InstallLegacySpellPresentation()
{
    if (!ConfigBool("WXL_M2_CHAOS_PRESENTATION",false)) return true;
    const auto* storage=Storage();
    if(!storage || storage->structSize<sizeof(WXL_StorageApi) || !storage->RegisterClientTransform) return false;
    g_poseTest=ConfigBool("WXL_M2_CHAOS_ANIMATIONS",false) && p::animationHooksReady.load();
    if(g_poseTest)storage->RegisterClientTransform("animationdata.dbc",&TransformAnimationData);
    storage->RegisterClientTransform("spellvisualkit.dbc",&TransformKit);
    storage->RegisterClientTransform("cfx_warlock_chaosbolt_impactchest.m2",&ObserveImpact);
    storage->RegisterClientTransform("cfx_warlock_chaosbolt_impactchest.mdx",&ObserveImpact);
    g_api->Subscribe(uint32_t(ev::Event::OnUpdate),&OnUpdate,nullptr);
    WLOG_INFO("spell-presentation-v1: opt-in release adapter installed; donor PTR12.1.0; poseTest=%u; start animation and sound projection deferred",unsigned(g_poseTest));
    return true;
}
