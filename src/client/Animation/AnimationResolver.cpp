// Extended playable-animation resolution for modern M2 models.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

#include "game/M2Animation.hpp"
#include "game/Unit.hpp"
#include "wxl/M2AnimationApi.h"
#include "client/Spell/PresentationAnimation.hpp"

#include <windows.h>

#include <cstdint>

namespace
{
    namespace animation = wxl::game::m2animation;

    using HasPlayableFn = bool(__thiscall*)(void* model, uint32_t animationId);
    using FindPlayableFn = int(__thiscall*)(void* model, uint32_t animationId);
    using ResolveAnimationFn = int(__thiscall*)(void* unit, int animationId, void* model);

    HasPlayableFn g_originalHasPlayable = nullptr;
    FindPlayableFn g_originalFindPlayable = nullptr;
    ResolveAnimationFn g_originalResolve = nullptr;
    WXL_M2AnimationResolveOverrideFn g_override = nullptr;

    bool g_faultReported = false;

    constexpr int kUnresolved = -1;
    constexpr int kMaxFallbackSteps = 16;

    namespace presentation=wxl::spell::presentation;
    uint32_t PoseRequest(void* model,uint32_t requested) noexcept
    {
        if(!presentation::animationProjectionActive.load() || presentation::PoseIndex(requested)<0)return requested;
        bool ready=false;
        __try
        {
            const auto* header=static_cast<const wxl::structure::m2::M2Header*>(animation::ModelData(model));
            if(header && header->sequences.count && header->sequences.count<=65536 && header->sequences.offset)
            {
                const auto* seqs=reinterpret_cast<const wxl::structure::m2::M2Sequence*>(header->sequences.offset);
                ready=presentation::EmbeddedPoseFamily({seqs,header->sequences.count}) &&
                    animation::ModelHasSequence(model,922) && animation::ModelHasSequence(model,926);
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER){ready=false;}
        const uint32_t selected=presentation::SelectPose(requested,ready);
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1)<24)WLOG_INFO("spell-animation-v1: model-query requested=%u selected=%u embeddedFamily=%u",requested,selected,unsigned(ready));
        return selected;
    }

    bool IsExtended(int animationId)
    {
        return animationId > 0 &&
               static_cast<uint32_t>(animationId) > animation::kLastStockId;
    }

    bool ModelHasSequenceGuarded(void* model, uint32_t animationId) noexcept
    {
        __try
        {
            return animation::ModelHasSequence(model, animationId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_faultReported)
            {
                g_faultReported = true;
                WLOG_WARN("m2-animation: model faulted answering for animation %u", animationId);
            }
            return false;
        }
    }

    int ResolveExtended(void* model, int animationId, int* stockFallback) noexcept
    {
        if (!model) return kUnresolved;
        if (ModelHasSequenceGuarded(model, static_cast<uint32_t>(animationId)))
            return animationId;

        __try
        {
            uint32_t current = static_cast<uint32_t>(animationId);
            for (int depth = 0; depth < kMaxFallbackSteps; ++depth)
            {
                const animation::AnimationRow* row = animation::Lookup(
                    current);
                if (!row || row->fallback == current) break;

                current = row->fallback;
                if (current <= animation::kLastStockId)
                {
                    if (stockFallback) *stockFallback = static_cast<int>(current);
                    break;
                }
                if (ModelHasSequenceGuarded(model, current))
                    return static_cast<int>(current);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return kUnresolved;
    }

    bool __fastcall HasPlayable(void* model, void*, uint32_t animationId)
    {
        animationId=PoseRequest(model,animationId);
        if (!IsExtended(static_cast<int>(animationId)))
            return g_originalHasPlayable(model, animationId);
        return ModelHasSequenceGuarded(model, animationId);
    }

    int __fastcall FindPlayable(void* model, void*, uint32_t animationId)
    {
        animationId=PoseRequest(model,animationId);
        if (!IsExtended(static_cast<int>(animationId)))
            return g_originalFindPlayable(model, animationId);
        return ModelHasSequenceGuarded(model, animationId)
            ? static_cast<int>(animationId) : kUnresolved;
    }

    int __fastcall ResolveAnimation(void* unit, void*, int animationId, void* model)
    {
        const int requested=animationId;
        if(presentation::animationProjectionActive.load() && presentation::PoseIndex(static_cast<uint32_t>(animationId))>=0)
        {
            void* effective=model;
            __try {if(!effective)effective=wxl::game::unit::Model(unit);}
            __except(EXCEPTION_EXECUTE_HANDLER){effective=nullptr;}
            animationId=static_cast<int>(PoseRequest(effective,static_cast<uint32_t>(animationId)));
        }
        int resolved = kUnresolved;
        if (IsExtended(animationId))
        {
            int stockFallback = kUnresolved;
            resolved = ResolveExtended(
                model ? model : wxl::game::unit::Model(unit), animationId,
                                       &stockFallback);
            if (resolved == kUnresolved && stockFallback != kUnresolved)
                resolved = g_originalResolve(unit, stockFallback, model);
        }
        else
        {
            resolved = g_originalResolve(unit, animationId, model);
        }

        if (g_override)
        {
            const int forced = g_override(unit, requested, model, resolved);
            if (forced >= 0) resolved=forced;
        }
        if(presentation::animationProjectionActive.load() && presentation::PoseIndex(static_cast<uint32_t>(requested))>=0)
        {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1)<24)WLOG_INFO("spell-animation-v1: requested=%d selected=%d resolved=%d nativeFallback=%u",requested,animationId,resolved,unsigned(animationId!=requested));
        }
        return resolved;
    }

    void __cdecl SetResolveOverride(WXL_M2AnimationResolveOverrideFn callback)
    {
        g_override = callback;
    }

    WXL_M2AnimationApi g_animationApi = {
        sizeof(WXL_M2AnimationApi),
        WXL_M2_ANIMATION_API_VERSION,
        &SetResolveOverride,
    };

    bool Attach(const char* pointName, void* detour, void** original)
    {
        return wxl_modern_m2::g_api->HookAttachByName(
            pointName, detour, original, WXL_HOOK_DEFAULT_PRIORITY) != 0;
    }
}

namespace wxl_modern_m2
{
    bool InstallExtendedAnimations()
    {
        bool ok = true;
        ok &= Attach("M2.HasPlayableAnimation", reinterpret_cast<void*>(&HasPlayable),
                     reinterpret_cast<void**>(&g_originalHasPlayable));
        ok &= Attach("M2.FindPlayableAnimation", reinterpret_cast<void*>(&FindPlayable),
                     reinterpret_cast<void**>(&g_originalFindPlayable));
        ok &= Attach("Unit.ResolveModelAnimation", reinterpret_cast<void*>(&ResolveAnimation),
                     reinterpret_cast<void**>(&g_originalResolve));
        if (!ok) return false;
        presentation::animationHooksReady.store(true);

        g_api->PublishInterface("wxl.m2-animation", WXL_M2_ANIMATION_API_VERSION,
                                &g_animationApi);
        WLOG_INFO("m2-animation: animation ids above %u resolved against the model",
                  animation::kLastStockId);
        return true;
    }
}
