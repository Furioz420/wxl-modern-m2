// m2: module bring-up + binding the core events to the M2 themes.
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

#include "ModernM2.hpp"

#include "AssetRegistry.hpp"
#include "BoneBudget.hpp"
#include "../ExtensionApi.hpp"
#include "../load/NativeLoad.hpp"
#include "Skin.hpp"

#include "engine/events/Event.hpp"
#include "game/M2.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "engine/assets/shared/models/m2/Contract.hpp"
#include "engine/assets/shared/models/m2/Particles.hpp"
#include "engine/assets/shared/models/m2/Ribbons.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wxl::modern::assets::m2
{
    namespace ev  = wxl::events;
    namespace m2  = wxl::game::m2;
    namespace fmt = wxl::structure::m2;
    namespace bn  = wxl::modern::assets::common::bones;

    namespace
    {
        common::AssetRegistry g_registry;

        constexpr uint32_t kTextureTransformStride = 0x3C;
        constexpr std::array<uint32_t, 3> kTextureTrackOffsets{
            0x00, 0x14, 0x28
        };
        constexpr uint32_t kMinTextureLoopMs = 100;
        constexpr uint32_t kMaxTextureLoopMs = 600000;

        // Repaired loop tables cannot be appended to the already-relocated
        // model arena. Retain one replacement per live model.
        std::mutex g_textureLoopMutex;
        std::unordered_map<void*, std::unique_ptr<uint32_t[]>>
            g_textureLoopTables;

        bool IsEquipmentPath(const char* path)
        {
            constexpr char kPrefix[] = "item\\objectcomponents\\";
            return path &&
                   _strnicmp(path, kPrefix, sizeof(kPrefix) - 1) == 0;
        }

        uint32_t TextureTrackDuration(const fmt::M2TrackHeader& track)
        {
            if (track.globalSequence < 0 || !track.timestamps.count ||
                !track.timestamps.offset || track.timestamps.count > 0x1000)
                return 0;

            const auto* outer = reinterpret_cast<const fmt::M2Array*>(
                static_cast<uintptr_t>(track.timestamps.offset));
            uint32_t duration = 0;
            for (uint32_t i = 0; i < track.timestamps.count; ++i)
            {
                if (outer[i].count < 2 || outer[i].count > 0x10000 ||
                    !outer[i].offset)
                    continue;
                const auto* timestamps = reinterpret_cast<const uint32_t*>(
                    static_cast<uintptr_t>(outer[i].offset));
                duration = std::max(
                    duration, timestamps[outer[i].count - 1]);
            }
            return duration >= kMinTextureLoopMs &&
                           duration <= kMaxTextureLoopMs
                       ? duration
                       : 0;
        }

        void ForgetTextureLoops(void* model)
        {
            const std::lock_guard lock(g_textureLoopMutex);
            g_textureLoopTables.erase(model);
        }

        /**
         * @brief Drops any registration left on this model pointer before the native reader fills it.
         *
         * The engine reuses model addresses: a pointer freed by one model can be handed straight back for
         * the next. Clearing here means the registry only ever holds live models, and the native reader
         * (RegisterNativeLoaded) is the one place that puts one back in.
         */
        void __cdecl OnModelLoadPre(void* /*user*/, const void* argsRaw)
        {
            const auto& a = *static_cast<const ev::ModelLoadArgs*>(argsRaw);
            // Model objects are reused. The outgoing header owns any two-phase palette map stored
            // during its skin rebuild, so discard it before the incoming model replaces the header.
            bn::ForgetPaletteMap(m2::M2Model(a.model).GetHeader());
            g_registry.Forget(a.model);
            ForgetTextureLoops(a.model);
            wxl::runtime::m2native::ReleaseSkeleton(a.model);
        }

        /**
         * @brief Splits any over-budget submesh, then rebuilds the material / texunit contract for the
         *        models the native MD21 reader filled.
         *
         * The bone-budget split (BoneBudget.hpp) is a hard client-engine constraint, not a format concern, so
         * it runs for every model whatever its origin. The shaderId decode + textureUnitLookup synth after it
         * is scoped to registered models only, because it assumes the modern packed shaderId encoding that
         * the native reader leaves on the live skin -- a stock v264 model already carries a resolved contract
         * and only needs the structural repoint when a split happened.
         */
        void __cdecl OnSkinFinalize(void* /*user*/, const void* argsRaw)
        {
            const auto& a = *static_cast<const ev::M2SkinFinalizeArgs*>(argsRaw);
            m2::M2Model model(a.model);
            auto* md = model.GetHeader();
            auto* sk = model.GetSkin();
            if (!md || !sk) return;

            std::vector<bn::SplitSection> sections;
            std::vector<bn::SplitRun> splitMap;
            uint32_t splitCount = 0;
            const char* pathStem = model.GetPathStem();
            const bool split = bn::SplitSubmeshes(md, sk, sections, splitMap, splitCount,
                                                  pathStem ? pathStem : "") && splitCount > 0;
            if (split)
                WLOG_INFO("modern-assets: bone-splitter produced %u extra sub-draw(s)", splitCount);

            if (g_registry.Contains(a.model))
                skin::Rebuild(md, sk, splitMap, pathStem ? pathStem : "");
            else if (split)
                bn::RepointBatchesAfterSplit(sk, splitMap);
        }

        /// Delegates the draw-time alpha-key fixup to the particles theme, flagged for reshaped models.
        void __cdecl OnSetupBatchAlpha(void* /*user*/, const void* argsRaw)
        {
            const auto& a = *static_cast<const ev::M2SetupBatchAlphaArgs*>(argsRaw);
            // Alpha-key batches are a small minority of the scene; test the blend mode before paying
            // the registry lookup, which otherwise costs a shared-lock + hash find on EVERY batch of
            // every visible model.
            if (a.blendMode != particles::kBlendAlphaKey) return;
            particles::OnSetupBatchAlpha(a, g_registry.Contains(a.model));
        }

        /// Delegates the ribbon draw fixup to the ribbons theme.
        void __cdecl OnRibbonDrawHandler(void* /*user*/, const void* argsRaw)
        {
            const auto& a = *static_cast<const ev::RibbonDrawArgs*>(argsRaw);
            ribbons::OnRibbonDraw(a);
        }
    }

    /**
     * @brief Registers a model the native MD21 reader direct-filled into this module's registry
     *        (kFlagHotReshaped: the packed modern shaderIds are present on the live skin, so the
     *        finalize-time contract rebuild and the draw fixups must scope to it).
     * @param model Runtime model pointer.
     */
    void RegisterNativeLoaded(void* model)
    {
        if constexpr (wxl_modern_m2::kEnabled)
            g_registry.Remember(model, common::AssetRegistry::kFlagHotReshaped);
        else
            (void)model;
    }

    /**
     * @brief Drops a native-reader registration (failed fill after registration).
     * @param model Runtime model pointer.
     */
    void ForgetNativeLoaded(void* model)
    {
        if constexpr (wxl_modern_m2::kEnabled)
        {
            g_registry.Forget(model);
            ForgetTextureLoops(model);
        }
        else
            (void)model;
    }

    bool IsNativeLoaded(void* model)
    {
        if constexpr (wxl_modern_m2::kEnabled)
            return g_registry.Contains(model);
        else
            return (void)model, false;
    }

    uint32_t RepairEquipmentTextureLoops(void* model,
                                         fmt::M2Header* header,
                                         const char* path)
    {
        if (!model || !header || !IsEquipmentPath(path) ||
            !header->textureTransforms.count ||
            !header->textureTransforms.offset)
            return 0;

        std::vector<uint32_t> loops;
        if (header->globalLoops.count && header->globalLoops.offset)
        {
            const auto* source = reinterpret_cast<const uint32_t*>(
                static_cast<uintptr_t>(header->globalLoops.offset));
            loops.assign(source, source + header->globalLoops.count);
        }

        struct Repair
        {
            fmt::M2TrackHeader* track;
            uint16_t loop;
        };
        std::vector<Repair> repairs;
        auto* transforms = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(header->textureTransforms.offset));
        for (uint32_t i = 0; i < header->textureTransforms.count; ++i)
        {
            uint8_t* transform =
                transforms + i * kTextureTransformStride;
            for (uint32_t offset : kTextureTrackOffsets)
            {
                auto* track = reinterpret_cast<fmt::M2TrackHeader*>(
                    transform + offset);
                if (track->globalSequence < 0 ||
                    track->globalSequence > 1)
                    continue;
                const uint32_t duration = TextureTrackDuration(*track);
                if (!duration) continue;

                uint16_t privateLoop = 0xFFFF;
                for (size_t n = 2; n < loops.size(); ++n)
                    if (loops[n] == duration)
                    {
                        privateLoop = static_cast<uint16_t>(n);
                        break;
                    }
                if (privateLoop == 0xFFFF)
                {
                    while (loops.size() < 2) loops.push_back(duration);
                    if (loops.size() >= 0xFFFE) continue;
                    loops.push_back(duration);
                    privateLoop = static_cast<uint16_t>(loops.size() - 1);
                }
                repairs.push_back({track, privateLoop});
            }
        }
        if (repairs.empty()) return 0;

        if (loops.size() != header->globalLoops.count)
        {
            auto replacement = std::make_unique<uint32_t[]>(loops.size());
            std::copy(loops.begin(), loops.end(), replacement.get());
            uint32_t* replacementData = replacement.get();
            {
                const std::lock_guard lock(g_textureLoopMutex);
                g_textureLoopTables[model] = std::move(replacement);
            }
            header->globalLoops.count = static_cast<uint32_t>(loops.size());
            header->globalLoops.offset = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(replacementData));
        }
        for (const Repair& repair : repairs)
            repair.track->globalSequence =
                static_cast<int16_t>(repair.loop);

        WLOG_INFO(
            "modern-assets: '%s' isolated %u equipment texture-transform loop(s)",
            path, static_cast<unsigned>(repairs.size()));
        return static_cast<uint32_t>(repairs.size());
    }
}

namespace wxl_modern_m2
{
    bool InstallModernM2()
    {
        namespace ev = wxl::events;
        namespace m2 = wxl::modern::assets::m2;


        g_api->Subscribe(uint32_t(ev::Event::OnModelLoadPre), &m2::OnModelLoadPre, nullptr);
        g_api->Subscribe(uint32_t(ev::Event::OnM2SkinFinalize), &m2::OnSkinFinalize, nullptr);
        g_api->Subscribe(uint32_t(ev::Event::OnM2SetupBatchAlpha), &m2::OnSetupBatchAlpha, nullptr);
        g_api->Subscribe(uint32_t(ev::Event::OnRibbonDraw), &m2::OnRibbonDrawHandler, nullptr);

        WLOG_INFO("modern-assets: m2 live-engine half loaded");
        return true;
    }
}
