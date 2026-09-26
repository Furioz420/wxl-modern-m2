#include "RetailHelmetPolicy.hpp"
// Native CharModel consumer for demand-resolved retail item-display object attachments.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "client/CharModel/RetailSkinProvider.hpp"
#include "client/FrameScript/WxlWow.hpp"
#include "client/Item/RetailItem.hpp"
#include "client/Item/NativeItemDbc.hpp"
#include "client/PrivateClientOffsets.hpp"
#include "ExtensionApi.hpp"
#include "compat/ModernM2.hpp"
#include "compat/CustomizationOwnership.hpp"
#include "compat/GilneanPreview.hpp"
#include "engine/assets/db2/ItemDisplayIndex.hpp"
#include "wxl/EventScript.hpp"
#include "game/Binding.hpp"
#include "game/M2.hpp"
#include "game/World.hpp"
#include "game/Unit.hpp"
#include "offsets/game/DB2.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/World.hpp"
#include "offsets/engine/Lua.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace gm2 = wxl::game::m2;
    namespace itemdisplay = wxl::runtime::db2::itemdisplay;
    namespace m2off = wxl::offsets::game::m2;
    namespace private_m2 = wxl_modern_m2::private_offsets::m2;
    namespace worldoff = wxl::offsets::game::world;
    namespace luaoff = wxl::offsets::engine::lua;

    bool IsInWorld() noexcept
    {
        return *reinterpret_cast<const int32_t*>(worldoff::kCurrentMapId) >= 0;
    }
    void* ActivePlayerComponent() noexcept
    {
        __try
        {
            if (!IsInWorld()) return nullptr;
            const auto guid = wxl::game::world::ActivePlayerGuid();
            return guid ? wxl::game::unit::CharacterComponent(
                wxl::game::world::ResolveObject(guid, wxl::game::world::kTypeMaskPlayer)) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    bool ActivePlayerEquipment(void* cmo, uint32_t slot, uint32_t& item,
                               uint32_t& modifier) noexcept
    {
        item = modifier = 0;
        if (!slot || slot > 19) return false;
        __try
        {
            const auto guid = wxl::game::world::ActivePlayerGuid();
            void* unit = guid ? wxl::game::world::ResolveObject(
                guid, wxl::game::world::kTypeMaskPlayer) : nullptr;
            if (!unit || wxl::game::unit::CharacterComponent(unit) != cmo) return false;
            // Build 12340's descriptor pointer also carries the GUID/type header.
            // PLAYER_VISIBLE_ITEM_1_ENTRYID = UNIT_END(0x94) + 0x87 = 0x11B.
            // Two DWORDs per visible slot; mainhand agrees with the existing 0x139 binding.
            constexpr uint32_t first = 0x11B;
            static_assert(first + 15 * 2 == wxl_modern_m2::private_offsets::unit::kVisibleItemMainhandEntry);
            const auto* fields = *reinterpret_cast<const uint32_t* const*>(
                static_cast<const uint8_t*>(unit) + wxl::offsets::game::unit::kObjectHeaderField);
            if (!fields) return false;
            const uint32_t encoded = fields[first + (slot - 1) * 2];
            if ((encoded & 0xC0000000u) == 0x80000000u)
            {
                item = encoded & 0xFFFFFu;
                modifier = (encoded >> 20) & 0x3FFu;
            }
            else
            {
                const int32_t value = static_cast<int32_t>(encoded);
                item = value < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(value)) : encoded;
                uint32_t variantItem = 0, variantModifier = 0;
                if (item && wxl::client::wxlwow::EquippedVariant(slot, variantItem, variantModifier) &&
                    variantItem == item) modifier = variantModifier;
            }
            return true; // A readable zero is unequipped; missing optional variant data is not.
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint32_t EquippedDragonDisplay(uint32_t item, uint32_t modifier) noexcept
    {
        if (!wxl::client::retailitem::IsNativePresentation(item))
            return wxl::client::retailitem::RequestDisplayForItem(item, modifier);
        // Use the existing native visible-item accessor for DBC-owned items.
        // RetailItem installs this same accessor; a complete native DBC pair
        // remains on its original lookup path instead of colliding with DB2.
        __try
        {
            const uint32_t visible[2]{item, 0};
            using DisplayFn = uint32_t(__fastcall*)(const uint32_t*, void*);
            return wxl::game::Native<DisplayFn>(0x00758E50)(visible, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Equipped armor on a dragon is attached geometry, never a humanoid body layer.
    bool DragonEquipmentSlot(uint32_t equipmentSlot)
    {
        return equipmentSlot == 3 || equipmentSlot == 6;
    }

    namespace db2off = wxl::offsets::game::db2;

    constexpr uint32_t kMissing = static_cast<uint32_t>(-1);
    constexpr uint32_t kMaxAttachmentWorkPerFrame = 2;
    constexpr uint32_t kMaxCustomizationAttachmentWorkPerFrame = 2;
    constexpr uint8_t kMaxModelAttachmentRetries = 3;
    constexpr uint8_t kMaxCustomizationAttachmentRetries = 8;
    constexpr uint32_t kCustomizationRetryBaseMs = 250;
    constexpr uint32_t kCustomizationRetryMaxMs = 5000;
    constexpr uint32_t kPendingSceneTimeoutMs = 30000;
    constexpr uint32_t kCustomizationRootAttachPoint = 19;
    std::atomic_uint32_t g_slotDiagnosticCount = 0;
    std::atomic_uint32_t g_displayApplyDiagnosticCount = 0;
    std::atomic_uint32_t g_attachmentDiagnosticCount = 0;
    std::atomic_uint32_t g_helmetDiagnosticCount = 0;
    std::atomic_uint32_t g_modelFallbackDiagnosticCount = 0;
    m2off::M2_CreateSceneModelFn g_originalCreateSceneModel = nullptr;
    m2off::M2_BindTexSlotFn g_originalBindTexSlot = nullptr;
    private_m2::CharacterGeosRenderPrepFn g_originalCharacterGeosRenderPrep =
        nullptr;

    bool ItemDetailLog()
    {
        static const bool enabled = wxl_modern_m2::ConfigBool(
            "WXL_M2_RETAIL_ITEM_DETAIL_LOG", false);
        return enabled;
    }

    bool CollectionRetargetEnabled()
    {
        static const bool enabled = wxl_modern_m2::ConfigBool(
            "WXL_M2_COLLECTION_RETARGET", true);
        return enabled;
    }

    struct PendingNativeMaterialBind
    {
        void* renderCtx = nullptr;
        uint32_t displayId = 0;
        uint32_t modelIndex = kMissing;
    };
    thread_local PendingNativeMaterialBind g_pendingNativeMaterialBind;
    thread_local bool g_replayingNativeMaterials = false;
    constexpr std::array<uint32_t, 19> kEquipToModelSlot{
        0, kMissing, 1, 2, 3, 4, 5, 6, 7, 8,
        kMissing, kMissing, kMissing, kMissing, 10,
        kMissing, kMissing, kMissing, 9,
    };
    constexpr std::array<std::array<uint32_t, 2>, 11> kModelSlotAttachPoints{{
        {{11, 55}}, {{6, 5}}, {{34, 34}}, {{34, 34}}, {{53, 53}}, {{9, 10}},
        {{47, 48}}, {{3, 4}}, {{1, 2}}, {{34, 34}}, {{12, 12}},
    }};
    // CharModel slots mapped back to the one-based FrameScript equipment
    // slots carried by the server-owned item-instance snapshot.
    constexpr std::array<uint32_t, 11> kModelToEquipmentSlot{
        1, 3, 4, 5, 6, 7, 8, 9, 10, 19, 15
    };

    struct PendingSlot
    {
        void* cmo = nullptr;
        void* sceneIdentity = nullptr;
        uint32_t modelSlot = kMissing;
        uint32_t displayId = 0;
        uint32_t postFlag = 0;
        uint32_t raceId = 0;
        uint32_t retailRaceId = 0;
        uint32_t genderId = 0;
        uint32_t createdAtMs = 0;
        uint32_t sceneMissingSinceMs = 0;
        bool applied = false;
        bool modelRetryRequired = false;
        uint32_t componentRetryAtMs = 0;
        uint8_t componentRetries = 0;
        uint8_t liveRepairAttempts = 0;
        uint32_t liveRepairAtMs = 0;
        std::string suppressedVariant;
    };

    struct AttachedSlot
    {
        uint32_t modelSlot = kMissing;
        struct BoneRemap
        {
            uint16_t count = 0;
            uint16_t keyMatches = 0;
            uint16_t nameMatches = 0;
            uint16_t inheritedMatches = 0;
            uint16_t uniqueSources = 0;
            uint16_t collectionToCharacter[256]{}; // Parent skeleton indices can exceed 255.
            float collectionPivot[256][3]{};
            float characterPivot[256][3]{};
            float geometryScale = 1.0f;
            void* collectionModel = nullptr;
            void* characterModel = nullptr;
        };
        struct Model
        {
            uint32_t attachId = kMissing;
            void* renderCtx = nullptr;
            void* characterCtx = nullptr;
            BoneRemap remap{};
            bool rootSkinned = false;
            bool cloneRemapLogged = false;
            std::string collectionVariant;
        };
        std::vector<Model> models;
    };

    struct CustomizationAttachmentGroup
    {
        uint32_t fileDataId = 0;
        std::vector<uint16_t> geosets;
        uint32_t retryAtMs = 0;
        uint8_t attempts = 0;
    };

    struct CustomizationAttachmentRequest
    {
        void* rootInstance = nullptr;
        uint32_t chrModel = 0;
        uint32_t generation = 0;
        std::vector<CustomizationAttachmentGroup> groups;
    };

    struct CustomizationAttachmentState
    {
        void* rootInstance = nullptr;
        uint32_t chrModel = 0;
        uint32_t generation = 0;
        std::vector<CustomizationAttachmentGroup> groups;
        size_t nextGroup = 0;
        std::vector<AttachedSlot::Model> models;
        uint32_t liveRepairWindow = 0;
        uint32_t liveRepairs = 0;
        uint32_t liveRepairAt = 0;
    };

    bool ReadU32(const void* address, uint32_t& value) noexcept
    {
        if (!address) return false;
        __try
        {
            value = *static_cast<const uint32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            value = 0;
            return false;
        }
    }

    void* ReadPtr(const void* address) noexcept
    {
        if (!address) return nullptr;
        __try
        {
            return *static_cast<void* const*>(address);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void* FindAncestorWithModel(void* instance, void* wantedModel) noexcept
    {
        if (!instance || !wantedModel) return nullptr;
        void* seen[32]{};
        void* current = ReadPtr(
            static_cast<uint8_t*>(instance) + m2off::kOffInstParent);
        for (uint32_t depth = 0; current && depth < 32; ++depth)
        {
            for (uint32_t i = 0; i < depth; ++i)
                if (seen[i] == current) return nullptr;
            seen[depth] = current;
            if (ReadPtr(static_cast<uint8_t*>(current) + m2off::kOffInstModel) ==
                wantedModel)
                return current;
            current = ReadPtr(
                static_cast<uint8_t*>(current) + m2off::kOffInstParent);
        }
        return nullptr;
    }

    uint32_t BoneCount(void* renderCtx) noexcept
    {
        if (!renderCtx) return 0;
        __try
        {
            void* model = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(renderCtx) + m2off::kOffInstModel);
            auto* header = model ? *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(model) + m2off::kOffModelHeader) : nullptr;
            const uint32_t count = header ? *reinterpret_cast<uint32_t*>(
                header + m2off::kOffHdrBoneCount) : 0;
            return count <= 1024 ? count : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    AttachedSlot::BoneRemap BuildBoneRemap(
        void* collectionCtx, void* characterCtx) noexcept
    {
        AttachedSlot::BoneRemap remap{};
        std::memset(remap.collectionToCharacter, 0xFF,
                    sizeof(remap.collectionToCharacter));
        __try
        {
            void* collectionModel = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(collectionCtx) + m2off::kOffInstModel);
            void* characterModel = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(characterCtx) + m2off::kOffInstModel);
            if (!collectionModel || !characterModel) return remap;
            remap.collectionModel = collectionModel;
            remap.characterModel = characterModel;

            auto* collectionHeader = *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(collectionModel) + m2off::kOffModelHeader);
            auto* characterHeader = *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(characterModel) + m2off::kOffModelHeader);
            if (!collectionHeader || !characterHeader) return remap;

            const uint32_t collectionCount = *reinterpret_cast<uint32_t*>(
                collectionHeader + m2off::kOffHdrBoneCount);
            auto* collectionBones = *reinterpret_cast<uint8_t**>(
                collectionHeader + m2off::kOffHdrBoneArray);
            const uint32_t characterCount = *reinterpret_cast<uint32_t*>(
                characterHeader + m2off::kOffHdrBoneCount);
            auto* characterBones = *reinterpret_cast<uint8_t**>(
                characterHeader + m2off::kOffHdrBoneArray);
            const uint32_t lutCount = *reinterpret_cast<uint32_t*>(
                characterHeader + m2off::kOffHdrBoneIdxLutCount);
            auto* lut = *reinterpret_cast<int16_t**>(
                characterHeader + m2off::kOffHdrBoneIdxLutPtr);
            if (!collectionBones || !characterBones || !collectionCount ||
                collectionCount > 256 || !characterCount || characterCount > 1024)
                return remap;

            remap.count = static_cast<uint16_t>(collectionCount);
            for (uint32_t i = 0; i < collectionCount; ++i)
            {
                const int32_t key = *reinterpret_cast<int32_t*>(
                    collectionBones + i * m2off::kBoneStride + m2off::kOffBoneKeyId);
                if (key < 0) continue;
                for (uint32_t j = 0; j < characterCount; ++j)
                {
                    const int32_t candidate = *reinterpret_cast<int32_t*>(
                        characterBones + j * m2off::kBoneStride + m2off::kOffBoneKeyId);
                    if (candidate != key) continue;
                    remap.collectionToCharacter[i] = static_cast<uint16_t>(j);
                    break;
                }
                if (remap.collectionToCharacter[i] == 0xFFFF && lut &&
                    static_cast<uint32_t>(key) < lutCount)
                {
                    const int16_t candidate = lut[key];
                    if (candidate >= 0 && static_cast<uint32_t>(candidate) < characterCount &&
                        candidate < 1024)
                        remap.collectionToCharacter[i] = static_cast<uint16_t>(candidate);
                }
                if (remap.collectionToCharacter[i] != 0xFFFF)
                    ++remap.keyMatches;
            }

            // Collection M2s intentionally keep many deformation bones out
            // of the small canonical key-bone table. Both legacy and modern
            // M2 records carry the CRC32 of the source bone name specifically
            // for cross-skeleton remapping. Resolve those names before the
            // lossy parent fallback; otherwise whole boot, knee, glove, and
            // shoulder chains collapse onto one static ancestor.
            for (uint32_t i = 0; i < collectionCount; ++i)
            {
                if (remap.collectionToCharacter[i] != 0xFFFF) continue;
                const uint32_t nameCrc = *reinterpret_cast<uint32_t*>(
                    collectionBones + i * m2off::kBoneStride +
                    m2off::kOffBoneNameCrc);
                if (!nameCrc) continue;
                for (uint32_t j = 0; j < characterCount; ++j)
                {
                    const uint32_t candidate = *reinterpret_cast<uint32_t*>(
                        characterBones + j * m2off::kBoneStride +
                        m2off::kOffBoneNameCrc);
                    if (candidate != nameCrc) continue;
                    remap.collectionToCharacter[i] = static_cast<uint16_t>(j);
                    ++remap.nameMatches;
                    break;
                }
            }
            for (uint32_t pass = 0; pass < collectionCount; ++pass)
            {
                bool changed = false;
                for (uint32_t i = 0; i < collectionCount; ++i)
                {
                    if (remap.collectionToCharacter[i] != 0xFFFF) continue;
                    const int16_t parent = *reinterpret_cast<int16_t*>(
                        collectionBones + i * m2off::kBoneStride + m2off::kOffBoneParent);
                    if (parent < 0 || static_cast<uint32_t>(parent) >= collectionCount ||
                        remap.collectionToCharacter[parent] == 0xFFFF)
                        continue;
                    remap.collectionToCharacter[i] =
                        remap.collectionToCharacter[parent];
                    ++remap.inheritedMatches;
                    changed = true;
                }
                if (!changed) break;
            }
            bool matched = false;
            for (uint32_t i = 0; i < collectionCount; ++i)
                if (remap.collectionToCharacter[i] != 0xFFFF)
                {
                    matched = true;
                    break;
                }
            if (!matched) remap.count = 0;
            if (matched)
            {
                bool sources[1024]{};
                for (uint32_t i = 0; i < collectionCount; ++i)
                {
                    const uint16_t source = remap.collectionToCharacter[i];
                    if (source == 0xFFFF || sources[source]) continue;
                    sources[source] = true;
                    ++remap.uniqueSources;
                }
                float collectionMin[3]{FLT_MAX, FLT_MAX, FLT_MAX};
                float collectionMax[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
                float characterMin[3]{FLT_MAX, FLT_MAX, FLT_MAX};
                float characterMax[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
                uint32_t pivotMatches = 0;
                for (uint32_t i = 0; i < collectionCount; ++i)
                {
                    const uint16_t sourceIndex =
                        remap.collectionToCharacter[i];
                    if (sourceIndex == 0xFFFF || sourceIndex >= characterCount)
                        continue;
                    const float* collectionPivot = reinterpret_cast<const float*>(
                        collectionBones + i * m2off::kBoneStride +
                        m2off::kOffBonePivot);
                    const float* characterPivot = reinterpret_cast<const float*>(
                        characterBones + sourceIndex * m2off::kBoneStride +
                        m2off::kOffBonePivot);
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        remap.collectionPivot[i][axis] = collectionPivot[axis];
                        remap.characterPivot[i][axis] = characterPivot[axis];
                    }

                    // Only canonical matches contribute to the scale estimate.
                    // Parent-inherited helper bones often share one character
                    // bone and would skew the bounds toward a single joint.
                    const int32_t key = *reinterpret_cast<int32_t*>(
                        collectionBones + i * m2off::kBoneStride +
                        m2off::kOffBoneKeyId);
                    if (key < 0) continue;
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        collectionMin[axis] = (std::min)(collectionMin[axis], collectionPivot[axis]);
                        collectionMax[axis] = (std::max)(collectionMax[axis], collectionPivot[axis]);
                        characterMin[axis] = (std::min)(characterMin[axis], characterPivot[axis]);
                        characterMax[axis] = (std::max)(characterMax[axis], characterPivot[axis]);
                    }
                    ++pivotMatches;
                }
                if (pivotMatches >= 4)
                {
                    float collectionExtentSq = 0.0f;
                    float characterExtentSq = 0.0f;
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        const float collectionExtent =
                            collectionMax[axis] - collectionMin[axis];
                        const float characterExtent =
                            characterMax[axis] - characterMin[axis];
                        collectionExtentSq += collectionExtent * collectionExtent;
                        characterExtentSq += characterExtent * characterExtent;
                    }
                    if (collectionExtentSq > 0.0001f &&
                        characterExtentSq > 0.0001f)
                    {
                        const float scale = std::sqrt(
                            characterExtentSq / collectionExtentSq);
                        remap.geometryScale = (std::clamp)(scale, 0.55f, 1.45f);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            remap.count = 0;
        }
        return remap;
    }

    void ApplyCharacterRetargetPolicy(AttachedSlot::BoneRemap& remap,
                                      void* cmo) noexcept
    {
        if (!remap.count || !cmo) return;
        uint32_t raceId = 0;
        uint32_t genderId = 0;
        ReadU32(static_cast<uint8_t*>(cmo) + m2off::kOffCmoRace, raceId);
        ReadU32(static_cast<uint8_t*>(cmo) + m2off::kOffCmoGender, genderId);

        // Custom Zandalari uses the legacy Troll animation skeleton. The
        // female Retail collection/legacy bind-pose extent ratio resolves to
        // 1.15, but scaling every skinned section around its individual bone
        // moves the visible pieces away from their otherwise-correct mapped
        // pivots. Keep the per-bone pivot retarget and use authored geometry
        // scale for this one mixed-skeleton contract, matching the working
        // male path.
        if (raceId == 18 && genderId == 1)
            remap.geometryScale = 1.0f;
    }

    bool BoneRemapMatches(const AttachedSlot::BoneRemap& remap,
                          void* collectionCtx, void* characterCtx) noexcept
    {
        if (!remap.count || !collectionCtx || !characterCtx ||
            collectionCtx == characterCtx ||
            !remap.collectionModel || !remap.characterModel)
            return false;
        return ReadPtr(static_cast<uint8_t*>(collectionCtx) +
                       m2off::kOffInstModel) == remap.collectionModel &&
               ReadPtr(static_cast<uint8_t*>(characterCtx) +
                       m2off::kOffInstModel) == remap.characterModel;
    }

    bool CopyBonePalette(void* collectionCtx, void* characterCtx,
                         const AttachedSlot::BoneRemap& remap) noexcept
    {
        const bool retarget = CollectionRetargetEnabled() &&
            !wxl::modern::assets::m2::IsNativeLoaded(remap.characterModel);
        if (!collectionCtx || !characterCtx || !remap.count ||
            remap.count != BoneCount(collectionCtx))
            return false;
        __try
        {
            if (*reinterpret_cast<void**>(
                    static_cast<uint8_t*>(collectionCtx) + m2off::kOffInstModel) !=
                    remap.collectionModel ||
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(characterCtx) + m2off::kOffInstModel) !=
                    remap.characterModel)
                return false;
            auto* destination = *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(collectionCtx) + m2off::kOffInstBonePalette);
            auto* source = *reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(characterCtx) + m2off::kOffInstBonePalette);
            const uint32_t sourceCount = BoneCount(characterCtx);
            if (!destination || !source || !sourceCount) return false;
            for (uint32_t i = 0; i < remap.count; ++i)
            {
                const uint16_t sourceIndex = remap.collectionToCharacter[i];
                if (sourceIndex == 0xFFFF || sourceIndex >= sourceCount) continue;
                auto* destinationMatrix = reinterpret_cast<float*>(
                    destination + i * m2off::kBonePaletteStride);
                const auto* sourceMatrix = reinterpret_cast<const float*>(
                    source + sourceIndex * m2off::kBonePaletteStride);
                std::memcpy(destinationMatrix, sourceMatrix,
                            m2off::kBonePaletteStride);
                if (!retarget) continue;

                const float scale = remap.geometryScale;
                // WoW's D3D9 palette uses row-vector matrices. The linear
                // transform occupies the upper-left 3x3 and translation is
                // _41/_42/_43 (indices 12..14). Writing the pivot correction
                // into _14/_24/_34 made it inert for rendering: equipment
                // whose Retail bind pose happened to resemble the legacy
                // character looked acceptable, while Draenei, Void Elf, Orc
                // female, Tauren, and allied-race pieces stayed around their
                // authored Retail pivots.
                for (uint32_t column = 0; column < 3; ++column)
                {
                    float characterPivot = 0.0f;
                    float collectionPivot = 0.0f;
                    for (uint32_t row = 0; row < 3; ++row)
                    {
                        const float value = sourceMatrix[row * 4 + column];
                        characterPivot +=
                            remap.characterPivot[i][row] * value;
                        collectionPivot +=
                            (scale * remap.collectionPivot[i][row]) * value;
                        destinationMatrix[row * 4 + column] = value * scale;
                    }
                    destinationMatrix[12 + column] +=
                        characterPivot - collectionPivot;
                }
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool NativeDisplayExists(uint32_t displayId) noexcept
    {
        if (!displayId) return false;
        __try
        {
            const uint32_t minId = *reinterpret_cast<const uint32_t*>(db2off::itemdisplayinfo::kMinId);
            const uint32_t maxId = *reinterpret_cast<const uint32_t*>(db2off::itemdisplayinfo::kMaxId);
            void** table = *reinterpret_cast<void***>(db2off::itemdisplayinfo::kIdTable);
            return table && displayId >= minId && displayId <= maxId &&
                   table[displayId - minId] != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool NativeShoulderPair(uint32_t displayId) noexcept
    {
        if (!NativeDisplayExists(displayId)) return false;
        __try
        {
            const uint32_t minId = *reinterpret_cast<const uint32_t*>(db2off::itemdisplayinfo::kMinId);
            void** table = *reinterpret_cast<void***>(db2off::itemdisplayinfo::kIdTable);
            const auto* row = static_cast<const uint8_t*>(table[displayId - minId]);
            const char* left = *reinterpret_cast<const char* const*>(row + 0x04);
            const char* right = *reinterpret_cast<const char* const*>(row + 0x08);
            return left && *left && right && *right;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool NativeHelmetVisibility(uint32_t displayId, uint32_t sex, uint32_t& visibility) noexcept
    {
        // The client-owned row layout is not the on-disk 25-DWORD layout.
        // Read verified file fields rather than interpreting runtime string data as IDs.
        return wxl::client::nativeitemdbc::SupplementalHelmetVisibility(displayId, sex, visibility);
    }

    bool HelmetActorIdentity(void* cmo, void* instance, uint32_t& race, uint32_t& sex) noexcept
    {
        __try
        {
            if (wxl_modern_m2::AlternateFormIdentity(cmo, race, sex)) return race != 0;
            void* shared = *reinterpret_cast<void**>(static_cast<uint8_t*>(instance) + 0x2c);
            if (!shared) return false;
            const char* path = reinterpret_cast<const char*>(shared) + 0x3c;
            uint32_t clientRace = 0;
            if (!wxl_modern_m2::RaceOfModelPath(path, clientRace, sex) ||
                !wxl_modern_m2::RetailCharacterCanaryAllows(clientRace, sex)) return false;
            race = wxl_modern_m2::RetailCharacterRace(clientRace);
            return WXL::RetailHelmetActor(path, clientRace, race != 0,
                wxl::modern::assets::m2::IsNativeLoaded(shared));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint32_t ClientModelRace(uint32_t raceId);

    bool HelmetRuleMatches(const itemdisplay::HelmetGeosetRule& rule,
                           uint32_t modelRace) noexcept
    {
        if (rule.raceId) return rule.raceId == modelRace;
        // PTR 12.1 currently uses only selection 3 for non-race rows:
        // every playable race. Preserve the bit semantics for future rows.
        return modelRace &&
               (rule.raceBitSelection == 3 ||
                (rule.raceBitSelection & 3u) == 3u);
    }

    bool ApplyRetailHelmetGeosets(
        void* model, const std::vector<itemdisplay::HelmetGeosetRule>& rules,
        uint32_t modelRace) noexcept
    {
        if (!model || rules.empty() || !modelRace) return true;
        __try
        {
            const auto setVisible =
                wxl::game::Native<m2off::M2_SetGeometryVisibleFn>(
                    m2off::kSetGeometryVisible);
            for (const itemdisplay::HelmetGeosetRule& rule : rules)
            {
                if (!HelmetRuleMatches(rule, modelRace) ||
                    rule.hideGroup > 99)
                    continue;
                // Character geoset N occupies N01..N99. ID 0 is the
                // character's mandatory base mesh and must never be hidden
                // when retail asks to hide group 0 (hair).
                const uint32_t first = rule.hideGroup * 100u + 1u;
                setVisible(model, nullptr, first, first + 98u, 0);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ApplyRetailCharacterGeosets(void* model, uint32_t robeGeoset,
                                     uint32_t capeGeoset) noexcept
    {
        if (!model || (!robeGeoset && !capeGeoset)) return true;
        __try
        {
            const auto setVisible =
                wxl::game::Native<m2off::M2_SetGeometryVisibleFn>(
                    m2off::kSetGeometryVisible);
            if (robeGeoset)
            {
                setVisible(model, nullptr, 501, 599, 0);
                setVisible(model, nullptr, 902, 999, 0);
                setVisible(model, nullptr, 1100, 1199, 0);
                setVisible(model, nullptr, 1300, 1399, 0);
                setVisible(model, nullptr, 1301 + robeGeoset,
                           1301 + robeGeoset, 1);
            }
            if (capeGeoset)
            {
                setVisible(model, nullptr, 1500, 1599, 0);
                setVisible(model, nullptr, 1501 + capeGeoset,
                           1501 + capeGeoset, 1);
            }
            wxl::game::Native<m2off::M2_OptimizeVisibleGeometryFn>(
                m2off::kOptimizeVisibleGeometry)(model, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ApplyEquipmentGeometry(void* cmo)
    {
        if (!cmo) return;

        uint32_t chestDisplayId = 0;
        uint32_t capeDisplayId = 0;
        uint32_t headDisplayId = 0;
        auto* bytes = static_cast<uint8_t*>(cmo);
        if (!ReadU32(bytes + private_m2::kOffCmoHeadDisplay, headDisplayId) ||
            !ReadU32(bytes + private_m2::kOffCmoChestDisplay, chestDisplayId) ||
            !ReadU32(bytes + private_m2::kOffCmoCapeDisplay, capeDisplayId))
            return;
        void* model = ReadPtr(bytes + m2off::kOffCmoSceneNode);
        if (!model) return;

        const auto index = itemdisplay::Current();
        if (!index) return;

        uint32_t robeGeoset = 0;
        uint32_t capeGeoset = 0;
        const std::vector<itemdisplay::HelmetGeosetRule>* helmetRules =
            nullptr;
        uint32_t helmetVisId = 0;
        uint32_t modelRace = 0;
        uint32_t gender = 0;
        if (headDisplayId && HelmetActorIdentity(cmo, model, modelRace, gender))
        {
            // Native/custom DBC rows remain authoritative, even for a zero ID.
            // Do not replace their visibility data with a colliding DB2 display.
            if (NativeDisplayExists(headDisplayId))
                NativeHelmetVisibility(headDisplayId, gender, helmetVisId);
            else if (const auto display = index->displayRecords.find(headDisplayId);
                     display != index->displayRecords.end())
                helmetVisId = display->second.helmetVis[gender == 1 ? 1u : 0u];
            if (helmetVisId && index->helmetData)
            {
                const auto rules = index->helmetData->geosetsByVis.find(helmetVisId);
                if (rules != index->helmetData->geosetsByVis.end()) helmetRules = &rules->second;
            }
        }
        // Log once per owner/head/visibility combination rather than spending the entire
        // diagnostic budget on the login character before NPCs load.
        static std::unordered_map<void*, uint64_t> helmetTrace;
        const uint64_t traceKey = (uint64_t(headDisplayId) << 32) | helmetVisId;
        if (headDisplayId && helmetTrace.size() < 256 &&
            (!helmetTrace.contains(cmo) || helmetTrace[cmo] != traceKey)) {
            helmetTrace[cmo] = traceKey;
            uint32_t matched = 0;
            if (helmetRules) for (const auto& rule : *helmetRules)
                if (HelmetRuleMatches(rule, modelRace)) ++matched;
            WLOG_INFO("npc-helmet-vis-v2: cmo=%p instance=%p head=%u vis=%u race=%u sex=%u rules=%zu matched=%u",
                cmo, model, headDisplayId, helmetVisId, modelRace, gender,
                helmetRules ? helmetRules->size() : 0, matched);
        }
        if (chestDisplayId && !NativeDisplayExists(chestDisplayId))
        {
            const auto display = index->displayRecords.find(chestDisplayId);
            if (display != index->displayRecords.end() &&
                display->second.inventoryType == 20 &&
                display->second.geosets[2] < 99)
                robeGeoset = display->second.geosets[2];
        }
        if (capeDisplayId && !NativeDisplayExists(capeDisplayId))
        {
            const auto display = index->displayRecords.find(capeDisplayId);
            if (display != index->displayRecords.end() &&
                display->second.inventoryType == 16 &&
                display->second.geosets[0] < 99)
                capeGeoset = display->second.geosets[0];
        }

        const bool helmetApplied =
            !helmetRules ||
            ApplyRetailHelmetGeosets(model, *helmetRules, modelRace);
        const bool equipmentApplied =
            ApplyRetailCharacterGeosets(model, robeGeoset, capeGeoset);
        if (helmetRules && helmetApplied)
        {
            wxl::game::Native<m2off::M2_OptimizeVisibleGeometryFn>(
                m2off::kOptimizeVisibleGeometry)(model, nullptr);
            if (g_helmetDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed) < 32)
                WLOG_INFO(
                    "retail-equipment: helmet geosets display=%u vis=%u race=%u rules=%zu",
                    headDisplayId, helmetVisId, modelRace,
                    helmetRules->size());
        }
        if (!helmetApplied || !equipmentApplied)
            WLOG_WARN(
                "retail-equipment: character geoset repair faulted head=%u chest=%u cape=%u",
                headDisplayId, chestDisplayId, capeDisplayId);
    }

    void __fastcall CharacterGeosRenderPrep(void* cmo, void* edx)
    {
        if (g_originalCharacterGeosRenderPrep) g_originalCharacterGeosRenderPrep(cmo, edx);
        ApplyEquipmentGeometry(cmo);
    }

    void SafeDetach(void* scene, uint32_t attachId) noexcept
    {
        __try { gm2::DetachSlot(scene, attachId); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // 0x831630 stores the requested attachment ID at +0x50 and its model-specific
    // resolved lookup index at +0x54 (0xFFFF for forced root collections).
    constexpr size_t kRequestedAttachmentId = 0x50;

    bool LiveAttachmentPresent(void* root, void* wanted, uint32_t attachId) noexcept
    {
        if (!root || !wanted) return false;
        __try
        {
            void* child = *reinterpret_cast<void**>(static_cast<uint8_t*>(root) + m2off::kOffInstAttachedHead);
            for (uint32_t count = 0; child && count < 256; ++count)
            {
                if (child == wanted)
                    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(child) + kRequestedAttachmentId) == attachId;
                child = *reinterpret_cast<void**>(static_cast<uint8_t*>(child) + m2off::kOffInstAttachedNext);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }

    bool SafeDetachContext(void* parent, void* child, uint32_t expectedAttachId) noexcept
    {
        if (!parent || !child) return false;
        __try
        {
            void** link = reinterpret_cast<void**>(
                static_cast<uint8_t*>(parent) + m2off::kOffInstAttachedHead);
            void* current = *link;
            for (uint32_t depth = 0; current && depth < 256; ++depth)
            {
                void* next = *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(current) +
                    m2off::kOffInstAttachedNext);
                if (current == child)
                {
                    // Native composition can free a child and reuse its address for a
                    // different slot. A stale owner must not unlink that replacement.
                    if (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(current) +
                            kRequestedAttachmentId) != expectedAttachId)
                        return false;
                    *link = next;
                    if (next)
                        *reinterpret_cast<void***>(
                            static_cast<uint8_t*>(next) +
                            private_m2::kOffInstAttachedPrev) = link;
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(child) +
                        m2off::kOffInstParent) = nullptr;
                    *reinterpret_cast<uint32_t*>(
                        static_cast<uint8_t*>(child) +
                        m2off::kOffInstAttachSlot) = 0xFFFFu;
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(child) +
                        private_m2::kOffInstAttachedPrev) = nullptr;
                    *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(child) +
                        m2off::kOffInstAttachedNext) = nullptr;
                    gm2::ReleaseRenderCtx(child);
                    return true;
                }
                link = reinterpret_cast<void**>(
                    static_cast<uint8_t*>(current) +
                    m2off::kOffInstAttachedNext);
                current = next;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    void RequestCharacterAppearanceRebuild(void* component) noexcept
    {
        if (!component) return;
        __try
        {
            *(static_cast<uint8_t*>(component) + m2off::kOffCharComponentRebuild) |=
                m2off::kCharRebuildSheet | m2off::kCharRebuildGeosets;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool SafeClearCharacterSlot(void* cmo, uint32_t equipSlotWow) noexcept
    {
        if (!cmo || equipSlotWow >= kEquipToModelSlot.size()) return false;
        __try
        {
            wxl::game::Native<m2off::M2_SlotClearFn>(m2off::kCharModelSlotClear)(
                cmo, nullptr, equipSlotWow);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void* SafeGetRenderCtx(void* owner, void* key) noexcept
    {
        __try { return gm2::GetRenderCtx(owner, key); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void* SafeLoadResource(const char* path) noexcept
    {
        __try { return gm2::LoadResource(path); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void SafeReleaseRenderCtx(void* renderCtx) noexcept
    {
        if (!renderCtx) return;
        __try { gm2::ReleaseRenderCtx(renderCtx); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void SafeBindTexture(void* renderCtx, uint32_t textureType, void* texture) noexcept
    {
        __try { gm2::BindTexSlotType(renderCtx, textureType, texture); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool ContainsAsciiInsensitive(const char* text, const char* needle) noexcept
    {
        if (!text || !needle || !*needle) return false;
        for (; *text; ++text)
        {
            const char* lhs = text;
            const char* rhs = needle;
            while (*lhs && *rhs)
            {
                char a = *lhs;
                char b = *rhs;
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
                if (a != b) break;
                ++lhs;
                ++rhs;
            }
            if (!*rhs) return true;
        }
        return false;
    }

    void TraceSceneModelFallback(void* renderCtx, const char* requestedPath,
                                 const void* caller, void* scene) noexcept
    {
        __try
        {
            void* const model = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(renderCtx) + m2off::kOffInstModel);
            const char* const resolvedPath = model ? gm2::M2Model(model).GetPathStem() : nullptr;
            if (!ContainsAsciiInsensitive(requestedPath, "errorcube") &&
                !ContainsAsciiInsensitive(resolvedPath, "errorcube"))
                return;

            const uint32_t diagnostic =
                g_modelFallbackDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed);
            if (diagnostic < 64u)
                WLOG_WARN(
                    "m2-loader-fallback: requested='%s' resolved='%s' caller=%p scene=%p",
                    requestedPath, resolvedPath ? resolvedPath : "", caller, scene);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN(
                "m2-loader-fallback: inspection fault requested='%s' caller=%p scene=%p",
                requestedPath ? requestedPath : "", caller, scene);
        }
    }

    bool IsErrorCubeRenderCtx(void* renderCtx) noexcept
    {
        if (!renderCtx) return true;
        __try
        {
            void* const model = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(renderCtx) + m2off::kOffInstModel);
            const char* const resolvedPath =
                model ? gm2::M2Model(model).GetPathStem() : nullptr;
            return !resolvedPath ||
                   ContainsAsciiInsensitive(resolvedPath, "errorcube");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    bool SafeAttach(void* renderCtx, void* scene, uint32_t attachId,
                    bool forceAttach) noexcept
    {
        bool attached = false;
        __try
        {
            gm2::AttachToScene(renderCtx, scene, attachId, forceAttach);
            attached = LiveAttachmentPresent(scene, renderCtx, attachId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        // CreateSceneModel returns one local reference. Release it exactly once whether attaching
        // succeeded or faulted; a successful scene attachment owns its own retained reference.
        SafeReleaseRenderCtx(renderCtx);
        return attached;
    }

    uint32_t CustomizationRetryDelay(uint8_t attempts) noexcept
    {
        const uint32_t shift = attempts > 1 ? attempts - 1u : 0u;
        const uint32_t delay = kCustomizationRetryBaseMs << (shift < 5u ? shift : 5u);
        return delay < kCustomizationRetryMaxMs ? delay : kCustomizationRetryMaxMs;
    }

    bool IsRootSkinnedCollection(const itemdisplay::ModelEntry& entry)
    {
        return entry.attachId == 19;
    }

    bool BuildModelPath(const itemdisplay::ModelEntry& entry, char (&path)[264])
    {
        if (!entry.folder || !*entry.folder || !entry.model || !*entry.model) return false;
        const int written = std::snprintf(path, sizeof(path), "Item\\ObjectComponents\\%s\\%s",
                                          entry.folder, entry.model);
        return written > 0 && static_cast<size_t>(written) < sizeof(path);
    }

    const char* PathBasename(const char* path)
    {
        if (!path) return "";
        const char* basename = path;
        for (const char* cursor = path; *cursor; ++cursor)
            if (*cursor == '\\' || *cursor == '/') basename = cursor + 1;
        return basename;
    }

    bool SameModelStem(const char* left, const char* right)
    {
        left = PathBasename(left);
        right = PathBasename(right);
        while (*left && *right && *left != '.' && *right != '.')
        {
            if (std::tolower(static_cast<unsigned char>(*left)) !=
                std::tolower(static_cast<unsigned char>(*right)))
                return false;
            ++left;
            ++right;
        }
        return (!*left || *left == '.') && (!*right || *right == '.');
    }

    bool BuildTexturePath(const itemdisplay::ModelEntry& entry, char (&path)[264])
    {
        if (!entry.folder || !*entry.folder || !entry.texture || !*entry.texture) return false;
        const int written = std::snprintf(path, sizeof(path), "Item\\ObjectComponents\\%s\\%s.blp",
                                          entry.folder, entry.texture);
        return written > 0 && static_cast<size_t>(written) < sizeof(path);
    }

    bool BuildMaterialTexturePath(const itemdisplay::MaterialEntry& entry,
                                  char (&path)[264])
    {
        if (!entry.folder || !*entry.folder || !entry.texture || !*entry.texture) return false;
        const int written = std::snprintf(path, sizeof(path),
                                          "Item\\ObjectComponents\\%s\\%s.blp",
                                          entry.folder, entry.texture);
        return written > 0 && static_cast<size_t>(written) < sizeof(path);
    }

    uint32_t BaseModelRace(uint32_t raceId)
    {
        // ComponentModelFileData does not always publish an allied-race row when that race
        // deliberately reuses its parent skeleton and component suffix. The old name-based
        // owner obtained this fallback from ChrRaces' model prefix. Keep exact retail rows
        // authoritative, then use the same parent family when the graph has no exact row.
        switch (raceId)
        {
            // Client 22/26 are faction Pandaren; component meshes use neutral Retail 24.
            // Falling through with local 22 accidentally selects Retail Worgen equipment.
            case 22: case 26: return 24;
            case 12: return 22; // client Worgen -> Retail Worgen
            case 9: return 35;  // Custom Vulpera -> retail Vulpera
            case 13: return 27; // Custom Nightborne -> retail Nightborne
            case 15: return 29; // Custom Void Elf -> retail Void Elf
            case 18: return 31; // Custom Zandalari -> retail Zandalari
            case 20: return 11; // Custom Lightforged Draenei -> Draenei
            case 29: return 34; // Custom Dark Iron Dwarf -> retail Dark Iron
            case 30: return 11; // Lightforged Draenei -> Draenei
            case 31: return 32; // Custom Kul Tiran -> retail Kul Tiran
            case 34: return 3;  // Dark Iron Dwarf -> Dwarf
            case 36: return 2;  // Mag'har Orc -> Orc
            case 84: return 11; // Custom Eredar -> Draenei
            default: return raceId;
        }
    }

    uint32_t ClientModelRace(uint32_t raceId)
    {
        // These two private ChrRaces rows retain their old prefixes for server/UI compatibility,
        // but their character model families are deliberately replaced by the Retail races below.
        if (raceId == 24) return 3; // Earthen equipment fallback -> Dwarf
        if (raceId == 27) return 4; // Harronir equipment fallback -> Night Elf

        uint32_t fallback = BaseModelRace(raceId);
        __try
        {
            const uint32_t low =
                *reinterpret_cast<const uint32_t*>(db2off::chrraces::kMinId);
            const uint32_t high =
                *reinterpret_cast<const uint32_t*>(db2off::chrraces::kMaxId);
            void** records =
                *reinterpret_cast<void***>(db2off::chrraces::kIdTable);
            if (!records || raceId < low || raceId > high) return fallback;
            auto* record = static_cast<uint8_t*>(records[raceId - low]);
            const char* prefix = record
                ? *reinterpret_cast<const char**>(
                      record + db2off::chrraces::kOffRecordPrefix)
                : nullptr;
            if (!prefix) return fallback;

            // ChrRaces owns the client model family for custom races. Matching its prefix
            // reproduces the old equipment owner's behavior without teaching the retail
            // DB2 graph private server race IDs.
            if (_strnicmp(prefix, "Hum", 3) == 0 || _stricmp(prefix, "Hu") == 0) return 1;
            if (_strnicmp(prefix, "Orc", 3) == 0 || _stricmp(prefix, "Or") == 0) return 2;
            if (_strnicmp(prefix, "Dwa", 3) == 0 || _stricmp(prefix, "Dw") == 0) return 3;
            if (_strnicmp(prefix, "Nig", 3) == 0 || _stricmp(prefix, "Ni") == 0) return 4;
            if (_strnicmp(prefix, "Sco", 3) == 0 ||
                _strnicmp(prefix, "Und", 3) == 0 || _stricmp(prefix, "Sc") == 0) return 5;
            if (_strnicmp(prefix, "Tau", 3) == 0 || _stricmp(prefix, "Ta") == 0) return 6;
            if (_strnicmp(prefix, "Gno", 3) == 0 || _stricmp(prefix, "Gn") == 0) return 7;
            if (_strnicmp(prefix, "Tro", 3) == 0 || _stricmp(prefix, "Tr") == 0) return 8;
            if (_strnicmp(prefix, "Gob", 3) == 0 || _stricmp(prefix, "Go") == 0) return 9;
            if (_strnicmp(prefix, "Blo", 3) == 0 ||
                _stricmp(prefix, "be") == 0) return 10;
            if (_strnicmp(prefix, "Dra", 3) == 0 ||
                _strnicmp(prefix, "Lig", 3) == 0 || _stricmp(prefix, "Dr") == 0) return 11;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return fallback;
        }
        return fallback;
    }

    uint32_t RetailModelRace(uint32_t raceId)
    {
        // Equipment and body/customization composition must agree about the identity of a private
        // client ChrRaces row. Numeric fall-through is unsafe: for example local 22 is Pandaren
        // (Horde), while Retail 22 is Worgen. Keep the single verified matrix authoritative.
        return wxl_modern_m2::RetailCharacterRace(raceId);
    }

    int ModelMatchRank(const itemdisplay::ModelEntry& entry,
                       uint32_t raceId, uint32_t fallbackRace,
                       uint32_t genderId)
    {
        int rank = 0;
        if (entry.raceId)
        {
            // A custom race can occupy a legacy numeric ID while ChrRaces points
            // at a different client model family (Dracthyr -> Blood Elf). In that
            // case the client prefix owns the skeleton choice; the numerically
            // exact retail row describes the legacy race and is only fallback.
            if (entry.raceId == raceId) rank = 6;
            else if (entry.raceId == fallbackRace) rank = 4;
            else return -1;
        }
        if (entry.genderId != kMissing && entry.genderId != genderId) return -1;
        return rank + (entry.genderId == genderId ? 1 : 0);
    }

    bool IsPreferredModel(const std::vector<itemdisplay::ModelEntry>& entries,
                          const itemdisplay::ModelEntry& candidate,
                          uint32_t raceId, uint32_t fallbackRace,
                          uint32_t genderId)
    {
        const int candidateRank =
            ModelMatchRank(candidate, raceId, fallbackRace, genderId);
        if (candidateRank < 0) return false;
        for (const itemdisplay::ModelEntry& other : entries)
        {
            if (&other == &candidate || other.modelSlot != candidate.modelSlot ||
                other.attachId != candidate.attachId ||
                other.modelIndex != candidate.modelIndex)
                continue;
            const int otherRank =
                ModelMatchRank(other, raceId, fallbackRace, genderId);
            if (otherRank > candidateRank) return false;
            if (otherRank == candidateRank && other.model && candidate.model &&
                std::strcmp(other.model, candidate.model) < 0)
                return false;
        }
        return true;
    }

    int MaterialMatchRank(const itemdisplay::MaterialEntry& entry,
                          uint32_t raceId, uint32_t fallbackRace,
                          uint32_t genderId)
    {
        int rank = 0;
        if (entry.raceId)
        {
            if (entry.raceId == raceId) rank = 6;
            else if (entry.raceId == fallbackRace) rank = 4;
            else return -1;
        }
        if (entry.genderId != kMissing && entry.genderId != genderId) return -1;
        return rank + (entry.genderId == genderId ? 1 : 0);
    }

    void BindModelMaterials(void* renderCtx, uint32_t displayId,
                            uint32_t modelIndex,
                            const itemdisplay::Index& index,
                            uint32_t raceId, uint32_t fallbackRace,
                            uint32_t genderId)
    {
        const auto found = index.materials.find(displayId);
        if (found == index.materials.end()) return;

        // A replaceable type has one binding per instance. Select the most specific
        // race/gender candidate for each type; component body layers have no texture type and
        // remain owned by the native character compositor.
        std::array<const itemdisplay::MaterialEntry*, 32> selected{};
        std::array<int, 32> ranks{};
        ranks.fill(-1);
        for (const itemdisplay::MaterialEntry& material : found->second)
        {
            if (material.textureType == kMissing || material.textureType == 0 ||
                material.textureType >= selected.size())
                continue;
            if (material.modelIndex != kMissing &&
                material.modelIndex != modelIndex)
                continue;
            const int rank =
                MaterialMatchRank(material, raceId, fallbackRace, genderId);
            if (rank < 0 || rank < ranks[material.textureType]) continue;
            selected[material.textureType] = &material;
            ranks[material.textureType] = rank;
        }

        for (uint32_t textureType = 1; textureType < selected.size(); ++textureType)
        {
            const itemdisplay::MaterialEntry* material = selected[textureType];
            if (!material) continue;
            char texturePath[264]{};
            if (!BuildMaterialTexturePath(*material, texturePath)) continue;
            void* texture = SafeLoadResource(texturePath);
            if (!texture)
            {
                WLOG_WARN("retail-equipment: material load failed display=%u modelIndex=%u type=%u texture=%s",
                          displayId, modelIndex, textureType, texturePath);
                continue;
            }
            SafeBindTexture(renderCtx, textureType, texture);
            if (ItemDetailLog())
                WLOG_INFO("retail-equipment: material bound display=%u modelIndex=%u type=%u race=%u gender=%u texture=%s",
                          displayId, modelIndex, textureType,
                          material->raceId, material->genderId, texturePath);
        }
    }


    thread_local void* g_equipmentModelOwner = nullptr;
    bool RetailHelmetOwner(void* cmo) noexcept
    {
        if (!cmo) return false;
        __try {
            void* root=*reinterpret_cast<void**>(static_cast<uint8_t*>(cmo)+0x38);
            if(!root)return false;
            void* shared=*reinterpret_cast<void**>(static_cast<uint8_t*>(root)+0x2c);
            if(!shared)return false;
            const char* path=reinterpret_cast<const char*>(shared)+0x3c;
            uint32_t race=0,sex=0;
            if(!wxl_modern_m2::RaceOfModelPath(path,race,sex))return false;
            return WXL::RetailHelmetActor(path,race,
                wxl_modern_m2::RetailCharacterRace(race)!=0 &&
                wxl_modern_m2::RetailCharacterCanaryAllows(race,sex),
                wxl::modern::assets::m2::IsNativeLoaded(shared));
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool BuildRetailHelmetPath(void* cmo,const char* requested,char (&output)[264])
    {
        if(!RetailHelmetOwner(cmo))return false;
        const std::string alias=WXL::RetailHelmetAlias(requested);
        if(alias.empty()||alias.size()>=sizeof(output))return false;
        // Only route files delivered by the unmodified Retail helmet pack. Missing
        // and private/custom helmet names retain their existing asset resolution.
        static const std::string directory=[] {
            char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);
            std::string path=exe;return path.substr(0,path.find_last_of("\\/"))+"\\Data\\Patch-Z.MPQ\\";
        }();
        static thread_local std::unordered_map<std::string,bool> available;
        auto found=available.find(alias);
        if(found==available.end()) {
            DWORD attr=GetFileAttributesA((directory+alias).c_str());
            found=available.emplace(alias,attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_DIRECTORY)).first;
        }
        if(!found->second)return false;
        std::memcpy(output,alias.c_str(),alias.size()+1);
        return true;
    }

    void* __fastcall CreateSceneModel(void* scene, void* edx, void* rawPath,
                                      uint32_t zero)
    {
        const void* const caller = _ReturnAddress();
        const char* effectivePath = static_cast<const char*>(rawPath);
        char retailHelmet[264]{};
        if(BuildRetailHelmetPath(g_equipmentModelOwner,effectivePath,retailHelmet))
            effectivePath=retailHelmet;
        void* renderCtx =
            g_originalCreateSceneModel(
                scene, edx, const_cast<char*>(effectivePath), zero);
        g_pendingNativeMaterialBind = {};
        if (!renderCtx || !effectivePath) return renderCtx;

        // The stock loader returns errorcube.m2 instead of null on failure.
        // Preserve that behavior, but record requested and resolved paths so
        // the bad secondary spell attachment can be repaired at its source.
        TraceSceneModelFallback(renderCtx, effectivePath, caller, scene);

        const uint32_t displayId =
            wxl::client::retailitem::DisplayLookupContext();
        if (!displayId) return renderCtx;
        const auto index = itemdisplay::Current();
        if (!index) return renderCtx;
        const auto entries = index->models.find(displayId);
        if (entries == index->models.end()) return renderCtx;

        const char* path = effectivePath;
        for (const itemdisplay::ModelEntry& entry : entries->second)
        {
            // These entries are intentionally left to the stock weapon owner.
            if (entry.modelSlot != kMissing || entry.attachId != kMissing ||
                !entry.model || !SameModelStem(path, entry.model))
                continue;
            g_pendingNativeMaterialBind = {
                renderCtx, displayId, entry.modelIndex
            };
            wxl::client::retailitem::ClearDisplayLookupContext();
            if (ItemDetailLog())
                WLOG_INFO(
                    "retail-equipment: native model correlated display=%u modelIndex=%u path=%s",
                    displayId, entry.modelIndex, path);
            break;
        }
        return renderCtx;
    }

    // Opt-in observation only: do not change the model, texture, or native call order.
    bool g_npcTextureTrace = false;
    struct NamedTexture { void* handle; char path[264]; };
    NamedTexture g_namedTextures[4096]{};
    uint32_t g_namedTextureNext = 0;
    SRWLOCK g_namedTextureLock = SRWLOCK_INIT;
    std::atomic<uint32_t> g_npcTextureBindLogs{0};
    using TraceTextureCreateFn = void*(__cdecl*)(const char*, uint32_t, int*, uint32_t);
    TraceTextureCreateFn g_originalTraceTextureCreate = nullptr;

    void* __cdecl TraceTextureCreate(const char* path, uint32_t flags, int* status, uint32_t flags2)
    {
        void* result = g_originalTraceTextureCreate(path, flags, status, flags2);
        if (!result || !path) return result;
        // Track every successful named reference, including cache hits, so a recycled handle
        // cannot retain an earlier creature name. Storage is bounded and no resource is retained.
        AcquireSRWLockExclusive(&g_namedTextureLock);
        NamedTexture* entry = nullptr;
        for (auto& candidate : g_namedTextures)
            if (candidate.handle == result) { entry = &candidate; break; }
        if (!entry) entry = &g_namedTextures[g_namedTextureNext++ % 4096];
        entry->handle = result;
        std::snprintf(entry->path, sizeof(entry->path), "%s", path);
        ReleaseSRWLockExclusive(&g_namedTextureLock);
        return result;
    }

    void TraceNpcTextureBind(void* renderCtx, uint32_t key, void* texture) noexcept
    {
        if (!g_npcTextureTrace || !renderCtx || !texture ||
            (key != 1 && key != 2 && key != 6 && key != 11 && key != 12 && key != 13) ||
            g_npcTextureBindLogs.load(std::memory_order_relaxed) >= 2048) return;
        char path[264]{};
        AcquireSRWLockShared(&g_namedTextureLock);
        for (const auto& entry : g_namedTextures)
            if (entry.handle == texture) {
                std::memcpy(path, entry.path, sizeof(path));
                break;
            }
        ReleaseSRWLockShared(&g_namedTextureLock);
        __try {
            void* model = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(renderCtx) + m2off::kOffInstModel);
            const char* stem = model ? gm2::M2Model(model).GetPathStem() : nullptr;
            if (!ContainsAsciiInsensitive(stem, "character\\") &&
                !ContainsAsciiInsensitive(stem, "creature\\")) return;
            if (g_npcTextureBindLogs.fetch_add(1, std::memory_order_relaxed) < 2048)
                WLOG_INFO("npc-texture-bind-v1: instance=%p model=%p path='%s' slot=%u texture=%p source='%s' nativeModern=%u world=%u",
                    renderCtx, model, stem, key, texture, path[0] ? path : "(unnamed/composited)",
                    wxl::modern::assets::m2::IsNativeLoaded(model), IsInWorld());
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void __fastcall BindTexSlot(void* renderCtx, void* edx, uint32_t key,
                                void* texture)
    {
        g_originalBindTexSlot(renderCtx, edx, key, texture);
        TraceNpcTextureBind(renderCtx, key, texture);
        if (g_replayingNativeMaterials) return;

        const PendingNativeMaterialBind pending = g_pendingNativeMaterialBind;
        if (pending.renderCtx != renderCtx || key != 2) return;
        g_pendingNativeMaterialBind = {};

        const auto index = itemdisplay::Current();
        if (!index) return;
        g_replayingNativeMaterials = true;
        BindModelMaterials(renderCtx, pending.displayId, pending.modelIndex,
                           *index, 0, 0, kMissing);
        g_replayingNativeMaterials = false;
        if (ItemDetailLog())
            WLOG_INFO(
                "retail-equipment: native model materials replayed display=%u modelIndex=%u",
                pending.displayId, pending.modelIndex);
    }

    class RetailEquipment final : public wxl::ext::EventScript
    {
    public:
        RetailEquipment()
        {
            on<&RetailEquipment::OnUpdate>(ev::Event::OnUpdate);
            on<&RetailEquipment::OnBuildBonePalette>(ev::Event::OnBuildBonePalette);
            on<&RetailEquipment::OnItemDisplayApply>(ev::Event::OnItemDisplayApply);
            on<&RetailEquipment::OnItemSlotChange>(ev::Event::OnItemSlotChange);
            on<&RetailEquipment::OnItemSlotClear>(ev::Event::OnItemSlotClear);
            on<&RetailEquipment::OnWorldEnter>(ev::Event::OnWorldEnter);
            on<&RetailEquipment::OnWorldLeave>(ev::Event::OnWorldLeave);
            WLOG_INFO("retail-equipment: native CharModel consumer registered");
        }

        bool GlueEquipmentReady() const
        {
            if (IsInWorld() || !latestGlueCmo_ ||
                GetTickCount() - latestGlueChangeMs_ < 400u)
                return false;
            // Hidden equipment is intentionally absent, not still loading. Treat the Glue preview as
            // settled so the startup/character-switch guard never waits forever on suppressed slots.
            if (!glueGearVisible_) return true;
            uint32_t previewRace = 0, previewSex = 0;
            if (wxl_modern_m2::CustomizeIdentity(previewRace, previewSex) &&
                previewRace == 14 && previewSex == 1)
            {
                // Creature female Tuskarr has no wearable humanoid outfit to await.
                // Require the live preview root before accepting this special case.
                return ReadPtr(static_cast<uint8_t*>(latestGlueCmo_) + m2off::kOffCmoSceneNode) != nullptr;
            }
            if (rootBindingsDirty_ || !GlueShouldersReady()) return false;

            uint32_t pendingCount = 0;
            for (const PendingSlot& pending : pending_)
            {
                if (pending.cmo != latestGlueCmo_) continue;
                ++pendingCount;
                if (!pending.applied || pending.modelRetryRequired ||
                    pending.componentRetries || pending.componentRetryAtMs)
                    return false;
            }
            if (pendingCount < 2) return false;

            const auto attached = attached_.find(latestGlueCmo_);
            if (attached != attached_.end())
                for (const AttachedSlot& slot : attached->second)
                    for (const AttachedSlot::Model& model : slot.models)
                        if ((model.rootSkinned && !model.remap.count) ||
                            !LiveAttachmentPresent(model.characterCtx, model.renderCtx, model.attachId))
                            return false;
            return true;
        }

        mutable uint32_t repairTraceBudget_ = 0;

        bool SlotHasMissingLiveModels(const PendingSlot& pending) const
        {
            const auto found = attached_.find(pending.cmo);
            if (found == attached_.end()) return false;
            for (const AttachedSlot& slot : found->second)
            {
                if (slot.modelSlot != pending.modelSlot) continue;
                for (const auto& model : slot.models)
                    if (!LiveAttachmentPresent(model.characterCtx, model.renderCtx, model.attachId))
                    {
                        repairTraceBudget_ = 64;
                        void* head = ReadPtr(static_cast<uint8_t*>(model.characterCtx) + m2off::kOffInstAttachedHead);
                        WLOG_INFO("attachment-loss-detail: cmo=%p scene=%p child=%p head=%p display=%u slot=%u point=%u attempt=%u resolved=%u",
                            pending.cmo, model.characterCtx, model.renderCtx, head, pending.displayId,
                            pending.modelSlot, model.attachId, pending.liveRepairAttempts,
                            itemdisplay::Current()->resolvedDisplays.contains(pending.displayId) ? 1u : 0u);
                        return true;
                    }
            }
            return false;
        }

        bool GlueShouldersReady() const
        {
            if (!glueShoulderOwner_ || glueShoulderOwner_ != latestGlueCmo_) return true;
            uint32_t display = 0;
            ReadU32(static_cast<uint8_t*>(glueShoulderOwner_) + 0x42C, display);
            if (!display || display != glueShoulderDisplay_) return true;
            void* root = ReadPtr(static_cast<uint8_t*>(glueShoulderOwner_) + m2off::kOffCmoSceneNode);
            if (!root || root != glueShoulderRoot_) return false;
            bool left = false, right = false;
            void* child = ReadPtr(static_cast<uint8_t*>(root) + m2off::kOffInstAttachedHead);
            for (unsigned count = 0; child && count < 128; ++count)
            {
                uint32_t slot = 0;
                ReadU32(static_cast<uint8_t*>(child) + kRequestedAttachmentId, slot);
                left |= slot == 6;
                right |= slot == 5;
                child = ReadPtr(static_cast<uint8_t*>(child) + m2off::kOffInstAttachedNext);
            }
            return left && right;
        }

        bool GearVisible() const { return glueGearVisible_; }
        bool DragonSlotVisible(uint32_t equipmentSlot) const
        {
            return (equipmentSlot == 3 && dragonShouldersVisible_) ||
                   (equipmentSlot == 6 && dragonWaistVisible_);
        }
        void SetDragonGearVisible(bool shoulders, bool waist)
        {
            dragonShouldersVisible_ = shoulders;
            dragonWaistVisible_ = waist;
            dragonReplayDue_ = 0;
            dragonReplayDisplays_.fill(kMissing);
            linkedDisplays_.fill(kMissing);
        }
        void SyncGilneanEquipment(void* primary, void* alternate)
        {
            if (IsInWorld() || !primary || !alternate || primary == alternate) return;
            // Snapshot before ApplyDisplay/SlotClear can mutate pending_. DB2-only
            // displays may not have reached the native component's slot array yet.
            std::array<uint32_t,11> wanted{};
            for (uint32_t i=0;i<wanted.size();++i)
                ReadU32(static_cast<uint8_t*>(primary)+0x428+i*4,wanted[i]);
            for (const auto& pending:pending_)
                if (pending.cmo==primary && pending.modelSlot<wanted.size()) wanted[pending.modelSlot]=pending.displayId;
            if (linkedPrimary_!=primary || linkedAlternate_!=alternate || linkedVisible_!=glueGearVisible_)
            {
                linkedPrimary_=primary;linkedAlternate_=alternate;linkedVisible_=glueGearVisible_;
                linkedDisplays_.fill(kMissing);
            }
            uint32_t alternateRace=0, alternateSex=0;
            if (wxl_modern_m2::GilneanPreviewIdentity(alternate,alternateRace,alternateSex) &&
                (alternateRace==52 || alternateRace==70))
            {
                // Dragon bodies use their authored armor customization; equipped
                // shoulders and compatible waist attachments are mirrored into this preview.
                for (uint32_t i=0;i<wanted.size();++i)
                    if (!DragonSlotVisible(kModelToEquipmentSlot[i])) wanted[i]=0;
            }
            if (!glueGearVisible_) wanted.fill(0);
            for (uint32_t i=0;i<wanted.size();++i)
            {
                if (linkedDisplays_[i]==wanted[i]) continue;
                SafeClearCharacterSlot(alternate,kModelToEquipmentSlot[i]-1);
                if (wanted[i])
                    wxl::game::Native<private_m2::ApplyDisplayFn>(private_m2::kCharModelApplyDisplay)(
                        alternate,nullptr,i,wanted[i],0);
                linkedDisplays_[i]=wanted[i];
                RequestCharacterAppearanceRebuild(alternate);
            }
        }
        void ReleaseGilneanEquipment(void* alternate)
        {
            if (!alternate) return;
            for (uint32_t i=0;i<kModelToEquipmentSlot.size();++i)
                SafeClearCharacterSlot(alternate,kModelToEquipmentSlot[i]-1);
            std::erase_if(pending_,[&](const PendingSlot& p){return p.cmo==alternate;});
            attached_.erase(alternate);
            if (linkedAlternate_==alternate) { linkedAlternate_=nullptr;linkedPrimary_=nullptr; }
            rootBindingsDirty_=true;
        }

        bool SetGlueGearVisible(bool visible)
        {
            if (IsInWorld()) return false;
            glueGearVisible_ = visible;
            glueGearSheetRebuildPending_ = visible;
            wxl_modern_m2::SetCharacterSheetGearVisible(visible);
            if (!latestGlueCmo_) return true;

            std::vector<uint32_t> slots;
            for (PendingSlot& pending : pending_)
            {
                if (pending.cmo != latestGlueCmo_) continue;
                if (std::find(slots.begin(), slots.end(), pending.modelSlot) == slots.end())
                    slots.push_back(pending.modelSlot);
                pending.applied = false;
                pending.modelRetryRequired = false;
                pending.componentRetryAtMs = 0;
                pending.componentRetries = 0;
                pending.suppressedVariant.clear();
            }
            if (!visible)
            {
                // SlotClear removes both the 3D attachment and the baked torso/leg/boot component
                // layers. Its normal event handler also forgets the Retail display, so retain pending_
                // during this deliberate preview-only clear; Show Gear can then replay the same items.
                preservePendingSlotClear_ = true;
                uint32_t cleared = 0;
                for (const uint32_t slot : slots)
                {
                    if (slot >= kModelToEquipmentSlot.size()) continue;
                    const uint32_t oneBased = kModelToEquipmentSlot[slot];
                    if (oneBased && SafeClearCharacterSlot(latestGlueCmo_, oneBased - 1)) ++cleared;
                    else DetachModelSlot(latestGlueCmo_, slot);
                }
                preservePendingSlotClear_ = false;
                WLOG_INFO("retail-equipment: cleared %u native Glue equipment component slot(s)",
                          cleared);
            }

            // ToggleDress supplies the naked stock class data after visibility is disabled. Consume
            // it immediately on hide. Showing is different: the Retail component layers are replayed
            // asynchronously from pending_, so rebuilding here uploads a naked sheet and wins the race
            // against those later ApplyDisplay calls. OnUpdate requests one rebuild after every pending
            // Glue slot is complete instead.
            if (!visible)
                RequestCharacterAppearanceRebuild(wxl_modern_m2::CustomizeComponent());

            latestGlueChangeMs_ = GetTickCount();
            WLOG_INFO("retail-equipment: Glue preview gear %s cmo=%p slots=%u",
                      visible ? "shown" : "hidden", latestGlueCmo_,
                      static_cast<unsigned>(slots.size()));
            return true;
        }

        void QueueCustomizationAttachments(
            void* cmo, void* rootInstance, uint32_t chrModel, uint32_t generation,
            const wxl_modern_m2::CustomizationAttachmentSpec* attachments,
            uint32_t attachmentCount)
        {
            if (!cmo) return;
            // generation is a recipe hash (CharacterGeosets), not the Glue UI epoch.
            // The map below already keeps the latest request for each CMO. Reject recycled
            // roots, not recipes belonging to another independently rendered character.
            if (!wxl_modern_m2::customization::CurrentRoot(rootInstance,
                    ReadPtr(static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode)))
            {
                static uint32_t logged = 0;
                if (logged++ < 16)
                    WLOG_INFO("customization-owner-v1: rejected recycled root cmo=%p root=%p recipe=%u",
                              cmo, rootInstance, generation);
                return;
            }

            const auto sameKey = [&](const auto& value) {
                return wxl_modern_m2::customization::SameRecipe(
                    value.rootInstance, value.chrModel, value.generation,
                    rootInstance, chrModel, generation);
            };
            if (const auto pending = customizationAttachmentRequests_.find(cmo);
                pending != customizationAttachmentRequests_.end() &&
                sameKey(pending->second))
                return;
            if (const auto active = customizationAttachments_.find(cmo);
                active != customizationAttachments_.end() &&
                sameKey(active->second))
                return;

            CustomizationAttachmentRequest request{};
            request.rootInstance = rootInstance;
            request.chrModel = chrModel;
            request.generation = generation;
            request.groups.reserve(attachmentCount);
            for (uint32_t i = 0; attachments && i < attachmentCount; ++i)
            {
                const auto& attachment = attachments[i];
                if (!attachment.fileDataId || attachment.geoset > 0xFFFFu)
                    continue;
                auto group = std::find_if(
                    request.groups.begin(), request.groups.end(),
                    [&](const CustomizationAttachmentGroup& candidate) {
                        return candidate.fileDataId == attachment.fileDataId;
                    });
                if (group == request.groups.end())
                {
                    request.groups.push_back({attachment.fileDataId, {}, 0, 0});
                    group = request.groups.end() - 1;
                }
                const uint16_t geoset = static_cast<uint16_t>(attachment.geoset);
                if (std::find(group->geosets.begin(), group->geosets.end(), geoset) ==
                    group->geosets.end())
                    group->geosets.push_back(geoset);
            }
            customizationAttachmentRequests_.insert_or_assign(
                cmo, std::move(request));
        }

        bool CustomizationAttachmentsReady(void* cmo) const noexcept
        {
            if (customizationAttachmentRequests_.find(cmo) != customizationAttachmentRequests_.end()) return false;
            const auto found = customizationAttachments_.find(cmo);
            if (found == customizationAttachments_.end()) return false;
            const auto& state = found->second;
            if (ReadPtr(static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode) != state.rootInstance ||
                state.nextGroup != state.groups.size() || state.models.size() != state.groups.size()) return false;
            for (const auto& model : state.models)
                if (!LiveAttachmentPresent(state.rootInstance, model.renderCtx, model.attachId)) return false;
            return true;
        }

        void ForgetCharacterEquipment(void* cmo) noexcept
        {
            if (!cmo) return;
            const size_t before = pending_.size();
            std::erase_if(pending_, [cmo](const PendingSlot& slot) { return slot.cmo == cmo; });
            // Called before the native CMO destructor. Native code releases its equipment
            // children; drop our bookkeeping, never replay it on a recycled CMO address.
            attached_.erase(cmo);
            ReleaseCustomizationAttachments(cmo);
            for (auto& owners : variantSlotOwners_)
                for (auto& owner : owners)
                    if (owner.cmo == cmo) owner = {};
            if (latestGlueCmo_ == cmo)
            {
                latestGlueCmo_ = nullptr;
                glueGearSheetRebuildPending_ = false;
            }
            if (glueShoulderOwner_ == cmo)
            {
                glueShoulderOwner_ = glueShoulderRoot_ = nullptr;
                glueShoulderChecks_ = 0;
            }
            if (dragonReplayOwner_ == cmo)
            {
                dragonReplayOwner_ = dragonReplayRoot_ = nullptr;
                dragonReplayDue_ = 0;
                dragonReplayDisplays_.fill(kMissing);
            }
            if (linkedPrimary_ == cmo || linkedAlternate_ == cmo)
            {
                linkedPrimary_ = linkedAlternate_ = nullptr;
                linkedDisplays_.fill(kMissing);
            }
            rootBindingsDirty_ = true;
            if (before != pending_.size())
                WLOG_INFO("retail-equipment: destroyed owner cmo=%p retired wardrobe slots=%u",
                          cmo, unsigned(before - pending_.size()));
        }

        void ReleaseCustomizationAttachments(void* cmo) noexcept
        {
            customizationAttachmentRequests_.erase(cmo);
            if (auto it = customizationAttachments_.find(cmo); it != customizationAttachments_.end())
            {
                DetachCustomizationState(cmo, it->second);
                customizationAttachments_.erase(it);
            }
            rootBindingsDirty_ = true;
        }

        void ResetCustomizationAttachments() noexcept
        {
            const uint32_t pending =
                static_cast<uint32_t>(customizationAttachmentRequests_.size());
            const uint32_t active =
                static_cast<uint32_t>(customizationAttachments_.size());
            customizationAttachmentRequests_.clear();
            for (auto& [cmo, state] : customizationAttachments_)
                DetachCustomizationState(cmo, state);
            customizationAttachments_.clear();
            rootBindingsDirty_ = true;
            if (pending || active)
                WLOG_INFO(
                    "character-customization-attach: context reset pending=%u active=%u",
                    pending, active);
        }

        void PrepareNativeEquipmentSlot(void* cmo, uint32_t modelSlot)
        {
            if (replayingComponents_ || !cmo || modelSlot >= 11) return;
            ForgetVariantSlotOwner(cmo, modelSlot);
            ForgetPending(cmo, modelSlot);
            DetachModelSlot(cmo, modelSlot);
        }

        bool SuppressGlueRetailDisplay(void* cmo, uint32_t displayId) const noexcept
        {
            return displayId && GlueGearHiddenFor(cmo);
        }

    private:
        void* glueShoulderOwner_ = nullptr;
        void* glueShoulderRoot_ = nullptr;
        uint32_t glueShoulderDisplay_ = 0;
        uint32_t glueShoulderDue_ = 0;
        uint32_t glueShoulderChecks_ = 0;

        void RepairGlueShoulders()
        {
            if (!glueShoulderChecks_ || !RetryDue(GetTickCount(), glueShoulderDue_)) return;
            if (IsInWorld() || wxl_modern_m2::CustomizeComponent() != glueShoulderOwner_)
            {
                glueShoulderChecks_ = 0;
                return;
            }
            uint32_t currentDisplay = 0;
            ReadU32(static_cast<uint8_t*>(glueShoulderOwner_) + 0x42C, currentDisplay);
            void* root = ReadPtr(static_cast<uint8_t*>(glueShoulderOwner_) + m2off::kOffCmoSceneNode);
            if (currentDisplay != glueShoulderDisplay_ || !root || root != glueShoulderRoot_ || GlueGearHiddenFor(glueShoulderOwner_))
            {
                glueShoulderChecks_ = 0;
                return;
            }
            --glueShoulderChecks_;
            glueShoulderDue_ = GetTickCount() + 1000;
            bool left = false, right = false;
            void* child = ReadPtr(static_cast<uint8_t*>(root) + m2off::kOffInstAttachedHead);
            for (unsigned count = 0; child && count < 128; ++count)
            {
                uint32_t slot = 0;
                ReadU32(static_cast<uint8_t*>(child) + kRequestedAttachmentId, slot);
                left |= slot == 6;
                right |= slot == 5;
                child = ReadPtr(static_cast<uint8_t*>(child) + m2off::kOffInstAttachedNext);
            }
            if (left && right) return;
            replayingComponents_ = true;
            wxl::game::Native<private_m2::ApplyDisplayFn>(private_m2::kCharModelApplyDisplay)(
                glueShoulderOwner_, nullptr, 1, glueShoulderDisplay_, 0);
            replayingComponents_ = false;
            WLOG_INFO("retail-equipment: replayed incomplete native shoulder pair display=%u left=%u right=%u", glueShoulderDisplay_, left, right);
        }

        std::vector<PendingSlot> pending_;
        std::unordered_map<void*, std::vector<AttachedSlot>> attached_;
        std::unordered_map<void*, CustomizationAttachmentRequest>
            customizationAttachmentRequests_;
        std::unordered_map<void*, CustomizationAttachmentState>
            customizationAttachments_;
        std::unordered_map<void*, void*> rootOwnerByContext_;
        std::unordered_map<void*, std::vector<void*>> rootOwnersByModel_;
        bool rootBindingsDirty_ = true;
        bool replayingComponents_ = false;
        bool glueGearVisible_ = true;
        bool glueGearSheetRebuildPending_ = false;
        bool preservePendingSlotClear_ = false;
        void* dragonSlotClearOwner_ = nullptr;
        bool dragonShouldersVisible_ = true;
        bool dragonWaistVisible_ = true;
        void* dragonReplayOwner_ = nullptr;
        void* dragonReplayRoot_ = nullptr;
        uint32_t dragonReplayDue_ = 0;
        std::array<uint32_t, 11> dragonReplayDisplays_{};

        void ReconcileDragonEquipment()
        {
            const uint32_t now = GetTickCount();
            if (dragonReplayDue_ && !RetryDue(now, dragonReplayDue_)) return;
            dragonReplayDue_ = now + 250;
            void* cmo = ActivePlayerComponent();
            uint32_t race = 0, sex = 0;
            if (!cmo || !wxl_modern_m2::AlternateFormIdentity(cmo, race, sex) ||
                (race != 52 && race != 70))
            {
                dragonReplayOwner_ = dragonReplayRoot_ = nullptr;
                dragonReplayDisplays_.fill(kMissing);
                return;
            }
            void* root = ReadPtr(static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode);
            if (!root) return;
            if (cmo != dragonReplayOwner_ || root != dragonReplayRoot_)
            {
                dragonReplayOwner_ = cmo;
                dragonReplayRoot_ = root;
                dragonReplayDisplays_.fill(kMissing);
            }
            for (uint32_t slot : {1u, 4u})
            {
                uint32_t item = 0, modifier = 0, display = 0;
                if (!ActivePlayerEquipment(cmo, kModelToEquipmentSlot[slot], item, modifier))
                    continue;
                if (item && DragonSlotVisible(kModelToEquipmentSlot[slot]))
                {
                    display = EquippedDragonDisplay(item, modifier);
                    if (!display) continue; // DB2 is still resolving; do not cache absence.
                }
                const auto pending = std::find_if(pending_.begin(), pending_.end(),
                    [&](const PendingSlot& p) { return p.cmo == cmo && p.modelSlot == slot; });
                if (dragonReplayDisplays_[slot] == display &&
                    (!display || NativeDisplayExists(display) ||
                     (pending != pending_.end() && pending->displayId == display))) continue;

                // Form changes clear native slots and pending attachments. Rebuild only
                // from the current player's authoritative equipment, never a Glue snapshot.
                if (!display)
                {
                    SafeClearCharacterSlot(cmo, kModelToEquipmentSlot[slot] - 1);
                    ForgetPending(cmo, slot);
                    DetachModelSlot(cmo, slot);
                }
                else if (NativeDisplayExists(display))
                {
                    wxl::game::Native<private_m2::ApplyDisplayFn>(private_m2::kCharModelApplyDisplay)(
                        cmo, nullptr, slot, display, 0);
                }
                else
                {
                    auto& replay = Remember(cmo, slot, display, 0);
                    replay.applied = false;
                    replay.modelRetryRequired = false;
                    replay.componentRetryAtMs = 0;
                    replay.componentRetries = 0;
                    itemdisplay::Request(display);
                    // OnUpdate's bounded attachment queue performs the model-only apply.
                }
                dragonReplayDisplays_[slot] = display;
                WLOG_INFO("dragon-equipment: reconcile cmo=%p slot=%u item=%u modifier=%u display=%u visible=%u",
                    cmo, slot, item, modifier, display,
                    DragonSlotVisible(kModelToEquipmentSlot[slot]) ? 1u : 0u);
            }
        }
        uint32_t observedVariantGeneration_ = 0;
        void* latestGlueCmo_ = nullptr;
        void* linkedPrimary_ = nullptr;
        void* linkedAlternate_ = nullptr;
        bool linkedVisible_ = true;
        std::array<uint32_t,11> linkedDisplays_{};
        uint32_t latestGlueChangeMs_ = 0;

        bool GlueGearHiddenFor(void* cmo) const noexcept
        {
            uint32_t race=0,gender=0;
            return !IsInWorld() && !glueGearVisible_ &&
                (cmo == latestGlueCmo_ || wxl_modern_m2::GilneanPreviewIdentity(cmo,race,gender));
        }
        struct VariantSlotOwner
        {
            void* cmo = nullptr;
        };
        std::array<std::array<VariantSlotOwner, 4>, 11> variantSlotOwners_{};
        std::array<uint8_t, 11> variantSlotOwnerCursor_{};

        static bool ConfigureCustomizationGeosets(
            void* renderCtx, const std::vector<uint16_t>& selected) noexcept
        {
            if (!renderCtx) return false;
            __try
            {
                const auto setVisible =
                    wxl::game::Native<m2off::M2_SetGeometryVisibleFn>(
                        m2off::kSetGeometryVisible);
                uint16_t hiddenGroups[64]{};
                uint32_t hiddenCount = 0;
                void* const model = ReadPtr(
                    static_cast<uint8_t*>(renderCtx) +
                    m2off::kOffInstModel);
                const auto* skin = model
                    ? gm2::M2Model(model).GetSkin() : nullptr;
                const auto hideGroup = [&](uint16_t group) {
                    if (std::find(hiddenGroups, hiddenGroups + hiddenCount, group) !=
                        hiddenGroups + hiddenCount)
                        return;
                    if (hiddenCount < std::size(hiddenGroups))
                        hiddenGroups[hiddenCount++] = group;
                    const uint32_t first = group ? group * 100u : 1u;
                    setVisible(renderCtx, nullptr, first, group * 100u + 99u, 0);
                };
                // A child FDID can carry several groups even when this appearance selects rows from
                // only one of them. Clear every non-base group the child actually owns, then restore
                // the selected rows below; otherwise the unmentioned variants start all-visible.
                if (skin && skin->submeshes)
                {
                    for (uint32_t i = 0; i < skin->submeshCount; ++i)
                    {
                        const uint16_t geoset =
                            skin->submeshes[i].skinSectionId;
                        if (geoset) hideGroup(
                            static_cast<uint16_t>(geoset / 100u));
                    }
                }
                else
                    for (const uint16_t geoset : selected)
                        hideGroup(static_cast<uint16_t>(geoset / 100u));
                for (const uint16_t geoset : selected)
                    // External customization children use the exact section id from the recipe.
                    // A group base such as 100 is its first visible crystal/decoration variant, not
                    // the main-character convention where value zero often means "None".
                    if (geoset)
                        setVisible(renderCtx, nullptr, geoset, geoset, 1);
                wxl::game::Native<m2off::M2_OptimizeVisibleGeometryFn>(
                    m2off::kOptimizeVisibleGeometry)(renderCtx, nullptr);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void DetachCustomizationState(
            void* cmo, CustomizationAttachmentState& state)
        {
            uint32_t detached = 0;
            for (AttachedSlot::Model& model : state.models)
                if (model.renderCtx &&
                    SafeDetachContext(state.rootInstance, model.renderCtx, model.attachId))
                    ++detached;
            if (!state.models.empty())
                WLOG_INFO(
                    "character-customization-attach: detached cmo=%p model=%u generation=%u children=%u/%u",
                    cmo, state.chrModel, state.generation, detached,
                    static_cast<unsigned>(state.models.size()));
            state.models.clear();
            rootBindingsDirty_ = true;
        }

        void RepairMissingCustomizationChildren(void* cmo,
            CustomizationAttachmentState& state, uint32_t now)
        {
            bool missing = false;
            for (const auto& model : state.models)
                if (!LiveAttachmentPresent(state.rootInstance, model.renderCtx, model.attachId))
                {
                    missing = true;
                    break;
                }
            if (!missing) return;
            // Native login/equipment rebuilds can unlink all visuals without changing
            // either CMO or root address. Never reuse an unlinked (possibly freed) child.
            // Bound repeated repair if a different native pass keeps removing it.
            if (now - state.liveRepairWindow >= 10000u)
            {
                state.liveRepairWindow = now;
                state.liveRepairs = 0;
            }
            if (state.liveRepairs >= 3 ||
                (state.liveRepairAt && int32_t(now - state.liveRepairAt) < 0)) return;
            ++state.liveRepairs;
            state.liveRepairAt = now + 500u;
            WLOG_INFO("character-customization-attach: repairing missing children cmo=%p model=%u generation=%u attempt=%u",
                      cmo, state.chrModel, state.generation, state.liveRepairs);
            DetachCustomizationState(cmo, state); // releases only children still owned by this root
            state.nextGroup = 0;
            for (auto& group : state.groups)
            {
                group.attempts = 0;
                group.retryAtMs = 0;
            }
        }

        void ProcessCustomizationAttachments(uint32_t now)
        {
            // A recipe is published from the post-geoset CMO hook. Adopt every latest request first,
            // but spread model loads across frames so Earthen/Harronir's larger child sets cannot turn
            // one Glue customization click into a long synchronous stall.
            while (!customizationAttachmentRequests_.empty())
            {
                auto pending = customizationAttachmentRequests_.begin();
                void* const cmo = pending->first;
                CustomizationAttachmentRequest request =
                    std::move(pending->second);
                customizationAttachmentRequests_.erase(pending);

                if (!wxl_modern_m2::customization::CurrentRoot(request.rootInstance,
                    ReadPtr(static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode)))
                {
                    continue;
                }

                if (auto active = customizationAttachments_.find(cmo);
                    active != customizationAttachments_.end())
                {
                    DetachCustomizationState(cmo, active->second);
                    customizationAttachments_.erase(active);
                }

                CustomizationAttachmentState state{};
                state.rootInstance = request.rootInstance;
                state.chrModel = request.chrModel;
                state.generation = request.generation;
                state.groups = std::move(request.groups);
                state.models.reserve(state.groups.size());
                customizationAttachments_.insert_or_assign(
                    cmo, std::move(state));
                rootBindingsDirty_ = true;
            }

            // A recycled CMO can retain its address while +0x38 changes. Exact-child detach against
            // the root that accepted the model cannot remove equipment or another root-slot-19 child.
            for (auto it = customizationAttachments_.begin();
                 it != customizationAttachments_.end();)
            {
                void* const liveRoot = ReadPtr(
                    static_cast<uint8_t*>(it->first) +
                    m2off::kOffCmoSceneNode);
                if (liveRoot == it->second.rootInstance)
                {
                    RepairMissingCustomizationChildren(it->first, it->second, now);
                    ++it;
                    continue;
                }
                DetachCustomizationState(it->first, it->second);
                it = customizationAttachments_.erase(it);
            }

            uint32_t work = 0;
            for (auto& [cmo, state] : customizationAttachments_)
            {
                while (state.nextGroup < state.groups.size() &&
                       work < kMaxCustomizationAttachmentWorkPerFrame)
                {
                    void* const root = ReadPtr(
                        static_cast<uint8_t*>(cmo) +
                        m2off::kOffCmoSceneNode);
                    void* const owner = root
                        ? ReadPtr(static_cast<uint8_t*>(root) +
                                  m2off::kOffSceneNodeOwner)
                        : nullptr;
                    if (root != state.rootInstance || !owner)
                        break;

                    CustomizationAttachmentGroup& group =
                        state.groups[state.nextGroup];
                    if (group.retryAtMs &&
                        static_cast<int32_t>(now - group.retryAtMs) < 0)
                        break;
                    group.retryAtMs = 0;
                    ++work;
                    const char* const path =
                        wxl_modern_m2::ResolveModel(group.fileDataId);
                    // The native scene-model constructor takes a mutable path buffer even though it
                    // only reads the key. Keep the resolver's shared/static string immutable and
                    // supply the same bounded local buffer convention used by equipment models.
                    char loadPath[264]{};
                    void* renderCtx = nullptr;
                    if (path && *path)
                    {
                        const size_t pathLength = std::strlen(path);
                        if (pathLength + 1 <= sizeof(loadPath))
                        {
                            std::memcpy(loadPath, path, pathLength + 1);
                            renderCtx = SafeGetRenderCtx(owner, loadPath);
                        }
                    }
                    if (!renderCtx || IsErrorCubeRenderCtx(renderCtx) ||
                        !ConfigureCustomizationGeosets(
                            renderCtx, group.geosets))
                    {
                        SafeReleaseRenderCtx(renderCtx);
                        if (++group.attempts >=
                            kMaxCustomizationAttachmentRetries)
                        {
                            WLOG_WARN(
                                "character-customization-attach: load exhausted cmo=%p model=%u generation=%u fdid=%u path=%s",
                                cmo, state.chrModel, state.generation,
                                group.fileDataId, path ? path : "");
                            ++state.nextGroup;
                        }
                        else
                            group.retryAtMs =
                                now + CustomizationRetryDelay(group.attempts);
                        break;
                    }

                    // Customization materials are selected and loaded on the character root, but
                    // Earthen crystals/hair and Harronir hair/quills are separate root-skinned M2s.
                    // Replaceable texture bindings are per render context, so copying only geometry
                    // leaves those children on the loader's white placeholder. Replay every selected
                    // whole-texture type onto the new child before it enters the scene.
                    uint32_t materialBindings = 0;
                    for (uint32_t textureType = 0; textureType < 32; ++textureType)
                    {
                        void* const texture =
                            wxl_modern_m2::CustomizationWholeTexture(
                                state.rootInstance, textureType);
                        if (!texture) continue;
                        SafeBindTexture(renderCtx, textureType, texture);
                        ++materialBindings;
                    }

                    AttachedSlot::Model model{
                        kCustomizationRootAttachPoint, renderCtx, root, {},
                        true, false, {}
                    };
                    model.remap = BuildBoneRemap(renderCtx, root);
                    // A root-skinned child without a verified palette map does not merely sit in the
                    // wrong pose: its vertices stretch from whichever stale matrices happen to be in
                    // the instance, producing the screen-length arm/neck spikes seen in Glue. Never
                    // put that child into the scene. A later update retries after both models finish
                    // loading; a family with no compatible map fails closed.
                    if (!model.remap.count)
                    {
                        SafeReleaseRenderCtx(renderCtx);
                        if (++group.attempts >= kMaxCustomizationAttachmentRetries)
                        {
                            WLOG_WARN(
                                "character-customization-attach: remap exhausted cmo=%p model=%u generation=%u fdid=%u path=%s",
                                cmo, state.chrModel, state.generation,
                                group.fileDataId, path ? path : "");
                            ++state.nextGroup;
                        }
                        else
                            group.retryAtMs =
                                now + CustomizationRetryDelay(group.attempts);
                        break;
                    }
                    CopyBonePalette(renderCtx, root, model.remap);

                    if (!SafeAttach(
                            renderCtx, root,
                            kCustomizationRootAttachPoint, true))
                    {
                        if (++group.attempts >=
                            kMaxCustomizationAttachmentRetries)
                        {
                            WLOG_WARN(
                                "character-customization-attach: attach exhausted cmo=%p model=%u generation=%u fdid=%u path=%s",
                                cmo, state.chrModel, state.generation,
                                group.fileDataId, path ? path : "");
                            ++state.nextGroup;
                        }
                        else
                            group.retryAtMs =
                                now + CustomizationRetryDelay(group.attempts);
                        break;
                    }

                    state.models.push_back(std::move(model));
                    ++state.nextGroup;
                    rootBindingsDirty_ = true;
                    wxl_modern_m2::NotifyAppearanceVisualsChanged(cmo);
                    WLOG_INFO(
                        "character-customization-attach: attached cmo=%p model=%u generation=%u fdid=%u geosets=%u materials=%u child=%p remap=%u path=%s",
                        cmo, state.chrModel, state.generation,
                        group.fileDataId,
                        static_cast<unsigned>(group.geosets.size()),
                        materialBindings, renderCtx,
                        state.models.back().remap.count, path);
                }
                if (work >= kMaxCustomizationAttachmentWorkPerFrame)
                    break;
            }
        }

        void RebuildRootBindings()
        {
            const auto forEachRootModel = [&](auto&& visit) {
                for (auto& [cmo, slots] : attached_)
                    for (AttachedSlot& slot : slots)
                        for (AttachedSlot::Model& model : slot.models)
                            if (model.rootSkinned && model.renderCtx)
                                visit(cmo, model);
                for (auto& [cmo, state] : customizationAttachments_)
                    for (AttachedSlot::Model& model : state.models)
                        if (model.rootSkinned && model.renderCtx)
                            visit(cmo, model);
            };

            size_t rootCount = 0;
            std::unordered_map<void*, size_t> ownerCounts;
            forEachRootModel([&](void*, AttachedSlot::Model& model) {
                ++rootCount;
                if (model.remap.count && model.remap.collectionModel)
                    ++ownerCounts[model.remap.collectionModel];
            });

            rootOwnerByContext_.clear();
            rootOwnerByContext_.reserve(rootCount);
            // Preserve the per-model vectors and their capacity across scene
            // churn. Dense creature groups commonly share collection models;
            // repeatedly destroying and growing these vectors from the
            // bone-palette callback caused allocator churn in the hottest
            // render path.
            for (auto& [model, owners] : rootOwnersByModel_)
                owners.clear();
            rootOwnersByModel_.reserve(ownerCounts.size());
            for (const auto& [model, count] : ownerCounts)
                rootOwnersByModel_[model].reserve(count);

            forEachRootModel([&](void* cmo, AttachedSlot::Model& model) {
                rootOwnerByContext_.insert_or_assign(model.renderCtx, cmo);
                if (!model.remap.count || !model.remap.collectionModel)
                    return;
                auto& owners =
                    rootOwnersByModel_[model.remap.collectionModel];
                if (std::find(owners.begin(), owners.end(), cmo) ==
                    owners.end())
                    owners.push_back(cmo);
            });
            rootBindingsDirty_ = false;
        }

        void EnsureRootBindings()
        {
            if (rootBindingsDirty_) RebuildRootBindings();
        }

        void RepairPendingRootRemaps()
        {
            // Some collection models (notably the female BE/Orc/Tauren
            // variants) build their native palette only once. If that callback
            // lands while rootBindingsDirty_ is set, the render hook safely
            // skips all STL access and there is no second callback from which
            // to initialize the remap. Resolve those zero-remap attachments on
            // the ordinary update path, where rebuilding indexes is safe.
            const auto repair = [&](void* cmo, void* liveCharacter,
                                    AttachedSlot::Model& model,
                                    bool applyRetarget) {
                if (!model.rootSkinned || !model.renderCtx ||
                    model.remap.count)
                    return;

                void* poseSource = FindAncestorWithModel(
                    model.renderCtx, model.remap.characterModel);
                if (!poseSource)
                    poseSource = ReadPtr(
                        static_cast<uint8_t*>(model.renderCtx) +
                        m2off::kOffInstParent);
                if (!poseSource) poseSource = liveCharacter;
                if (!poseSource || poseSource == model.renderCtx)
                    return;

                AttachedSlot::BoneRemap rebuilt =
                    BuildBoneRemap(model.renderCtx, poseSource);
                if (!rebuilt.count) return;
                if (applyRetarget)
                    ApplyCharacterRetargetPolicy(rebuilt, cmo);
                model.remap = rebuilt;
                model.characterCtx = poseSource;
                CopyBonePalette(model.renderCtx, poseSource, model.remap);
                rootBindingsDirty_ = true;
                WLOG_INFO(
                    "%s: repaired deferred root pose child=%p character=%p remap=%u key=%u name=%u inherited=%u sources=%u scale=%.3f",
                    applyRetarget ? "retail-equipment" :
                                    "character-customization-attach",
                    model.renderCtx, poseSource, model.remap.count,
                    model.remap.keyMatches, model.remap.nameMatches,
                    model.remap.inheritedMatches,
                    model.remap.uniqueSources,
                    model.remap.geometryScale);
            };

            for (auto& [cmo, slots] : attached_)
            {
                void* liveCharacter = ReadPtr(
                    static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode);
                if (!liveCharacter) continue;
                for (AttachedSlot& slot : slots)
                    for (AttachedSlot::Model& model : slot.models)
                        repair(cmo, liveCharacter, model, true);
            }
            for (auto& [cmo, state] : customizationAttachments_)
            {
                void* liveCharacter = ReadPtr(
                    static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode);
                if (!liveCharacter ||
                    liveCharacter != state.rootInstance)
                    continue;
                for (AttachedSlot::Model& model : state.models)
                    repair(cmo, liveCharacter, model, false);
            }
        }

        void AddVariantSlotOwner(void* cmo, uint32_t modelSlot)
        {
            if (!cmo || modelSlot >= variantSlotOwners_.size()) return;
            auto& owners = variantSlotOwners_[modelSlot];
            for (VariantSlotOwner& owner : owners)
            {
                if (owner.cmo == cmo) return;
            }
            const uint8_t cursor =
                variantSlotOwnerCursor_[modelSlot]++ %
                static_cast<uint8_t>(owners.size());
            owners[cursor] = {cmo};
        }

        void ForgetVariantSlotOwner(void* cmo, uint32_t modelSlot)
        {
            if (!cmo || modelSlot >= variantSlotOwners_.size()) return;
            for (VariantSlotOwner& owner : variantSlotOwners_[modelSlot])
            {
                if (owner.cmo == cmo) owner = {};
            }
        }

        bool HasVariantSlotOwner(uint32_t modelSlot) const
        {
            if (modelSlot >= variantSlotOwners_.size()) return false;
            for (const VariantSlotOwner& owner :
                 variantSlotOwners_[modelSlot])
                if (owner.cmo) return true;
            return false;
        }

        void SeedVariantSlotOwner(
            uint32_t modelSlot, uint32_t displayId)
        {
            if (HasVariantSlotOwner(modelSlot) || !displayId) return;
            const PendingSlot* newest = nullptr;
            for (const PendingSlot& pending : pending_)
            {
                if (pending.modelSlot != modelSlot ||
                    pending.displayId != displayId ||
                    !ReadPtr(static_cast<uint8_t*>(pending.cmo) +
                             m2off::kOffCmoSceneNode))
                    continue;
                if (!newest ||
                    static_cast<int32_t>(
                        pending.createdAtMs - newest->createdAtMs) > 0)
                    newest = &pending;
            }
            if (!newest) return;
            AddVariantSlotOwner(newest->cmo, modelSlot);
            WLOG_INFO(
                "retail-equipment: variant owner seeded cmo=%p slot=%u display=%u",
                newest->cmo, modelSlot, displayId);
        }

        void RememberVariantSlotOwner(
            void* cmo, uint32_t modelSlot, uint32_t displayId)
        {
            if (!cmo || modelSlot >= variantSlotOwners_.size() ||
                !displayId)
                return;

            uint32_t itemId = 0;
            uint32_t modifierId = 0;
            if (!wxl::client::wxlwow::EquippedVariant(
                    kModelToEquipmentSlot[modelSlot],
                    itemId, modifierId))
                return;
            const uint32_t equippedDisplay =
                wxl::client::retailitem::RequestDisplayForItem(
                    itemId, modifierId);
            if (!equippedDisplay || equippedDisplay != displayId) return;

            // Once the world transition selected an owner for this slot,
            // keep it stable. A dense creature batch can legitimately carry
            // the same display, but must never evict the player correlation.
            if (IsInWorld())
            {
                const auto& owners = variantSlotOwners_[modelSlot];
                for (const VariantSlotOwner& owner : owners)
                {
                    if (owner.cmo == cmo) return;
                    if (owner.cmo) return;
                }
            }
            AddVariantSlotOwner(cmo, modelSlot);
        }

        bool ReconcileActivePlayerVariants(uint32_t generation)
        {
            if (generation == observedVariantGeneration_) return true;
            if (!IsInWorld())
            {
                observedVariantGeneration_ = generation;
                return true;
            }

            uint32_t changed = 0;
            const uint32_t now = GetTickCount();
            for (uint32_t modelSlot = 0;
                 modelSlot < kModelToEquipmentSlot.size(); ++modelSlot)
            {
                uint32_t itemId = 0;
                uint32_t modifierId = 0;
                if (!wxl::client::wxlwow::EquippedVariant(
                        kModelToEquipmentSlot[modelSlot],
                        itemId, modifierId))
                    continue;
                const uint32_t displayId =
                    wxl::client::retailitem::RequestDisplayForItem(
                        itemId, modifierId);
                if (!displayId) continue;
                // The first authoritative variant snapshot can arrive after
                // both Glue and world equipment dispatch. Seed from the newest
                // live matching CMO at that point; otherwise there is no
                // later item-field event for a same-item material change.
                SeedVariantSlotOwner(modelSlot, displayId);

                // A same-item normal/mythic swap first runs the native CMO
                // dispatch while the preceding server snapshot is still
                // current. Correlate the replacement snapshot with the
                // player owners captured at the Glue/world transition. The
                // set remains stable for the world lifetime so later dense
                // creature dispatch cannot evict the player.
                for (const VariantSlotOwner& owner :
                     variantSlotOwners_[modelSlot])
                {
                    if (!owner.cmo) continue;
                    const auto pending = std::find_if(
                        pending_.begin(), pending_.end(),
                        [&](const PendingSlot& slot) {
                            return slot.cmo == owner.cmo &&
                                   slot.modelSlot == modelSlot;
                        });
                    if (pending == pending_.end() ||
                        pending->displayId == displayId)
                        continue;

                    DetachModelSlot(pending->cmo, pending->modelSlot);
                    pending->displayId = displayId;
                    pending->applied = false;
                    pending->modelRetryRequired = false;
                    pending->componentRetryAtMs = 0;
                    pending->componentRetries = 0;
                    pending->suppressedVariant.clear();
                    pending->createdAtMs = now;

                    // The server snapshot arrives just after the native
                    // same-item slot dispatch. If the replacement graph is
                    // already resident, rebuild the affected player/preview
                    // owner now instead of leaving it behind unrelated
                    // creature work in the per-frame attachment queue.
                    if (const auto current = itemdisplay::Current();
                        current &&
                        current->resolvedDisplays.contains(displayId) &&
                        AttachResolved(*pending, *current))
                        MarkApplied(*pending, now);
                    ++changed;
                }
            }
            observedVariantGeneration_ = generation;
            if (changed)
                WLOG_INFO(
                    "retail-equipment: refreshed active-player variants generation=%u slots=%u",
                    generation, changed);
            return true;
        }

        void OnWorldEnter(const ev::WorldEnterArgs&)
        {
            // CWorld emits this after its native transition, so the world CMO
            // has already replayed its equipment. Rebuild the correlation
            // from live pending owners whose display matches the authoritative
            // equipped snapshot, then freeze it against creature churn.
            variantSlotOwners_ = {};
            variantSlotOwnerCursor_ = {};
            for (uint32_t modelSlot = 0;
                 modelSlot < kModelToEquipmentSlot.size(); ++modelSlot)
            {
                uint32_t itemId = 0;
                uint32_t modifierId = 0;
                if (!wxl::client::wxlwow::EquippedVariant(
                        kModelToEquipmentSlot[modelSlot],
                        itemId, modifierId))
                    continue;
                const uint32_t displayId =
                    wxl::client::retailitem::RequestDisplayForItem(
                        itemId, modifierId);
                if (!displayId) continue;
                SeedVariantSlotOwner(modelSlot, displayId);
            }
        }

        void OnWorldLeave(const ev::WorldLeaveArgs&)
        {
            dragonReplayOwner_ = dragonReplayRoot_ = nullptr;
            dragonReplayDue_ = 0;
            dragonReplayDisplays_.fill(kMissing);
            variantSlotOwners_ = {};
            variantSlotOwnerCursor_ = {};
            for (auto& [cmo, state] : customizationAttachments_)
                DetachCustomizationState(cmo, state);
            customizationAttachments_.clear();
            customizationAttachmentRequests_.clear();
            rootBindingsDirty_ = true;
        }

        AttachedSlot::Model* FindRootBinding(
            void* cmo, void* renderCtx,
            void* collectionModel = nullptr)
        {
            const auto found = attached_.find(cmo);
            if (found != attached_.end())
            {
                for (AttachedSlot& slot : found->second)
                {
                    for (AttachedSlot::Model& model : slot.models)
                    {
                        if (!model.rootSkinned) continue;
                        if (renderCtx && model.renderCtx != renderCtx) continue;
                        if (collectionModel &&
                            (!model.remap.count ||
                             model.remap.collectionModel != collectionModel))
                            continue;
                        return &model;
                    }
                }
            }

            const auto customization = customizationAttachments_.find(cmo);
            if (customization == customizationAttachments_.end())
                return nullptr;
            for (AttachedSlot::Model& model : customization->second.models)
            {
                if (!model.rootSkinned) continue;
                if (renderCtx && model.renderCtx != renderCtx) continue;
                if (collectionModel &&
                    (!model.remap.count ||
                     model.remap.collectionModel != collectionModel))
                    continue;
                return &model;
            }
            return nullptr;
        }

        void ForgetPending(void* cmo, uint32_t modelSlot)
        {
            std::erase_if(pending_, [&](const PendingSlot& slot) {
                return slot.cmo == cmo && slot.modelSlot == modelSlot;
            });
        }

        PendingSlot& Remember(void* cmo, uint32_t modelSlot, uint32_t displayId,
                              uint32_t postFlag = 0)
        {
            const uint32_t now = GetTickCount();
            uint32_t alternateRace=0,alternateGender=0;
            if (!IsInWorld() && !wxl_modern_m2::GilneanPreviewIdentity(cmo,alternateRace,alternateGender))
            {
                latestGlueCmo_ = cmo;
                latestGlueChangeMs_ = now;
            }
            void* scene = ReadPtr(static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode);
            uint32_t raceId = 0;
            uint32_t genderId = 0;
            ReadU32(static_cast<uint8_t*>(cmo) + m2off::kOffCmoRace, raceId);
            ReadU32(static_cast<uint8_t*>(cmo) + m2off::kOffCmoGender, genderId);
            uint32_t retailRaceId = RetailModelRace(raceId);
            wxl_modern_m2::AlternateFormIdentity(cmo,retailRaceId,genderId);
            bool reused = false;
            if (scene)
            {
                for (const PendingSlot& slot : pending_)
                {
                    if (slot.cmo == cmo && slot.sceneIdentity &&
                        slot.sceneIdentity != scene)
                    {
                        reused = true;
                        break;
                    }
                }
                if (!reused)
                {
                    const auto attached = attached_.find(cmo);
                    if (attached != attached_.end())
                        for (const AttachedSlot& slot : attached->second)
                            for (const AttachedSlot::Model& model : slot.models)
                                if (model.characterCtx && model.characterCtx != scene)
                                {
                                    reused = true;
                                    break;
                                }
                }
            }
            if (reused)
            {
                attached_.erase(cmo);
                rootBindingsDirty_ = true;
                for (PendingSlot& slot : pending_)
                {
                    if (slot.cmo != cmo) continue;
                    slot.sceneIdentity = scene;
                    slot.createdAtMs = now;
                    slot.sceneMissingSinceMs = 0;
                    slot.applied = false;
                    slot.modelRetryRequired = false;
                    slot.componentRetryAtMs = 0;
                    slot.componentRetries = 0;
                    slot.suppressedVariant.clear();
                }
                WLOG_INFO("retail-equipment: recycled cmo=%p scene=%p; retained wardrobe generation",
                          cmo, scene);
            }

            for (PendingSlot& slot : pending_)
            {
                if (slot.cmo != cmo || slot.modelSlot != modelSlot) continue;
                if (slot.displayId != displayId || slot.raceId != raceId ||
                    slot.retailRaceId != retailRaceId ||
                    slot.genderId != genderId)
                {
                    if (slot.raceId != raceId || slot.retailRaceId != retailRaceId ||
                        slot.genderId != genderId)
                        WLOG_INFO("retail-equipment: appearance generation changed cmo=%p slot=%u race=%u->%u retail=%u->%u gender=%u->%u",
                                  cmo, modelSlot, slot.raceId, raceId,
                                  slot.retailRaceId, retailRaceId,
                                  slot.genderId, genderId);
                    slot.createdAtMs = now;
                    slot.liveRepairAttempts = 0;
                    slot.liveRepairAtMs = 0;
                    slot.applied = false;
                    slot.modelRetryRequired = false;
                    slot.componentRetryAtMs = 0;
                    slot.componentRetries = 0;
                    slot.suppressedVariant.clear();
                }
                if (scene)
                {
                    slot.sceneIdentity = scene;
                    slot.sceneMissingSinceMs = 0;
                }
                slot.displayId = displayId;
                slot.postFlag = postFlag;
                slot.raceId = raceId;
                slot.retailRaceId = retailRaceId;
                slot.genderId = genderId;
                return slot;
            }
            pending_.push_back({
                cmo, scene, modelSlot, displayId, postFlag, raceId, retailRaceId, genderId,
                now, 0, false, false, 0, 0
            });
            if (ItemDetailLog())
                WLOG_INFO("retail-equipment: pending cmo=%p slot=%u display=%u",
                          cmo, modelSlot, displayId);
            return pending_.back();
        }

        static bool RetryDue(uint32_t now, uint32_t deadline) noexcept
        {
            return static_cast<int32_t>(now - deadline) >= 0;
        }

        static uint32_t RetryDelay(const PendingSlot& pending) noexcept
        {
            const uintptr_t identity = reinterpret_cast<uintptr_t>(pending.cmo);
            const uint32_t spread = static_cast<uint32_t>(
                ((identity >> 4) ^ (pending.displayId * 2654435761u) ^
                 pending.modelSlot) % 1000u);
            return 750u + spread;
        }

        void MarkApplied(PendingSlot& pending, uint32_t now = 0)
        {
            pending.applied = true;
            if (!pending.modelRetryRequired)
            {
                pending.componentRetryAtMs = 0;
                pending.componentRetries = 0;
                return;
            }

            // CharModel now resolves component-texture aliases synchronously
            // before the stock ApplyDisplay compositor runs. Replaying every
            // successful component slot later is therefore unnecessary and
            // visibly detaches/re-attaches collection sections while the user
            // is previewing a race. Only failed object-model/resource loads
            // retain bounded retries because those can race scene creation.
            pending.componentRetries = kMaxModelAttachmentRetries;
            pending.componentRetryAtMs = now ? now + RetryDelay(pending) : 0u;
        }

        void DetachModelSlot(void* cmo, uint32_t modelSlot)
        {
            const auto found = attached_.find(cmo);
            if (found == attached_.end()) return;
            if (repairTraceBudget_)
            {
                --repairTraceBudget_;
                WLOG_INFO("attachment-detach-detail: cmo=%p slot=%u replaying=%u", cmo, modelSlot, replayingComponents_ ? 1u : 0u);
            }
            void* scene = ReadPtr(static_cast<uint8_t*>(cmo) + m2off::kOffCmoSceneNode);
            std::vector<std::string> releasedVariants;
            bool removed = false;
            for (size_t i = 0; i < found->second.size();)
            {
                AttachedSlot& slot = found->second[i];
                if (slot.modelSlot != modelSlot)
                {
                    ++i;
                    continue;
                }
                if (scene)
                    for (const AttachedSlot::Model& model : slot.models)
                    {
                        if (model.characterCtx == scene)
                            SafeDetachContext(scene, model.renderCtx, model.attachId);
                        if (!model.collectionVariant.empty())
                            releasedVariants.push_back(model.collectionVariant);
                }
                found->second.erase(found->second.begin() + static_cast<ptrdiff_t>(i));
                removed = true;
            }
            if (found->second.empty()) attached_.erase(found);
            if (removed) rootBindingsDirty_ = true;

            if (releasedVariants.empty()) return;
            for (PendingSlot& pending : pending_)
            {
                if (pending.cmo != cmo || pending.suppressedVariant.empty() ||
                    std::find(releasedVariants.begin(), releasedVariants.end(),
                              pending.suppressedVariant) ==
                        releasedVariants.end())
                    continue;
                if (ItemDetailLog())
                    WLOG_INFO(
                        "retail-equipment: waking suppressed slot=%u variant=%s",
                        pending.modelSlot, pending.suppressedVariant.c_str());
                pending.suppressedVariant.clear();
                pending.applied = false;
                pending.modelRetryRequired = false;
                pending.componentRetryAtMs = 0;
                pending.componentRetries = 0;
            }
        }

        bool ClaimCollectionVariant(PendingSlot& pending, void* scene,
                                    const char* variant)
        {
            if (!variant || !*variant) return true;
            const auto found = attached_.find(pending.cmo);
            if (found == attached_.end()) return true;

            for (size_t slotIndex = 0; slotIndex < found->second.size();
                 ++slotIndex)
            {
                AttachedSlot& slot = found->second[slotIndex];
                for (size_t modelIndex = 0; modelIndex < slot.models.size();
                     ++modelIndex)
                {
                    AttachedSlot::Model& model = slot.models[modelIndex];
                    if (!model.rootSkinned ||
                        model.collectionVariant != variant)
                        continue;

                    // Lower model-slot numbers have deterministic ownership.
                    // In particular a robe/chest (slot 3) owns a shared lower
                    // section over legs (slot 5), independent of equip order.
                    if (slot.modelSlot <= pending.modelSlot)
                    {
                        pending.suppressedVariant = variant;
                        if (ItemDetailLog())
                            WLOG_INFO(
                                "retail-equipment: suppressed duplicate slot=%u owner=%u variant=%s",
                                pending.modelSlot, slot.modelSlot, variant);
                        return false;
                    }

                    for (PendingSlot& displaced : pending_)
                    {
                        if (displaced.cmo != pending.cmo ||
                            displaced.modelSlot != slot.modelSlot)
                            continue;
                        displaced.suppressedVariant = variant;
                        break;
                    }
                    if (scene && model.characterCtx == scene)
                        SafeDetachContext(scene, model.renderCtx, model.attachId);
                    if (ItemDetailLog())
                        WLOG_INFO(
                            "retail-equipment: replaced duplicate owner=%u slot=%u variant=%s",
                            slot.modelSlot, pending.modelSlot, variant);
                    slot.models.erase(
                        slot.models.begin() + static_cast<ptrdiff_t>(modelIndex));
                    if (slot.models.empty())
                        found->second.erase(
                            found->second.begin() +
                            static_cast<ptrdiff_t>(slotIndex));
                    if (found->second.empty()) attached_.erase(found);
                    rootBindingsDirty_ = true;
                    return true;
                }
            }
            return true;
        }

        bool ReplayNativeComponents(const PendingSlot& pending,
                                    const itemdisplay::Index& index)
        {
            const auto found = index.displayRecords.find(pending.displayId);
            if (found == index.displayRecords.end()) return false;

            bool applied = false;
            replayingComponents_ = true;
            __try
            {
                // Run the complete native display owner. The DBC-first lookup hook supplies the
                // resolved retail row, and ApplyDisplay performs skin-composition bookkeeping
                // that a direct call to the lower SlotDispatch owner omits.
                wxl::game::Native<private_m2::ApplyDisplayFn>(
                    private_m2::kCharModelApplyDisplay)(
                        pending.cmo, nullptr, pending.modelSlot, pending.displayId,
                        pending.postFlag);
                applied = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                WLOG_WARN("retail-equipment: component replay fault display=%u slot=%u",
                          pending.displayId, pending.modelSlot);
            }
            replayingComponents_ = false;
            if (applied && ItemDetailLog())
                WLOG_INFO("retail-equipment: component replay display=%u slot=%u",
                          pending.displayId, pending.modelSlot);
            return applied;
        }

        static void DetachNativeSlotFallback(void* scene, uint32_t modelSlot)
        {
            if (!scene || modelSlot >= kModelSlotAttachPoints.size()) return;
            // Glove models use hand attachment points 1/2, which are also the native weapon
            // owner's right/left points. Our tracked retail glove models are already detached by
            // DetachModelSlot; clearing these points generically removes a valid WotLK weapon.
            if (modelSlot == 8) return;
            const auto& points = kModelSlotAttachPoints[modelSlot];
            SafeDetach(scene, points[0]);
            if (points[1] != points[0]) SafeDetach(scene, points[1]);
        }

        static bool IsWeaponInventoryType(uint32_t inventoryType)
        {
            switch (inventoryType)
            {
                case 13: case 14: case 15: case 17:
                case 21: case 22: case 25: case 26:
                    return true;
                default:
                    return false;
            }
        }

        bool AttachResolved(PendingSlot& pending, const itemdisplay::Index& index,
                            bool modelsOnly = false)
        {
            struct Timing
            {
                void* cmo; uint32_t slot, display; DWORD start = GetTickCount();
                ~Timing() {
                    const DWORD elapsed = GetTickCount() - start;
                    if (elapsed >= 8) WLOG_INFO("equipment-attach-timing: cmo=%p slot=%u display=%u elapsed_ms=%u",
                        cmo, slot, display, elapsed);
                }
            } timing{pending.cmo, pending.modelSlot, pending.displayId};
            const uint32_t diagnostic =
                g_attachmentDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed);
            const bool logAttempt = diagnostic < 64 || modelsOnly || pending.cmo == dragonReplayOwner_ || ItemDetailLog();
            void* scene = ReadPtr(static_cast<uint8_t*>(pending.cmo) + m2off::kOffCmoSceneNode);
            void* owner = scene
                ? ReadPtr(static_cast<uint8_t*>(scene) + m2off::kOffSceneNodeOwner) : nullptr;
            if (!scene || !owner)
            {
                if (logAttempt)
                    WLOG_INFO("retail-equipment: display=%u pose deferred scene=%p owner=%p",
                              pending.displayId, scene, owner);
                return false;
            }
            pending.modelRetryRequired = false;

            uint32_t raceId = 0;
            uint32_t genderId = 0;
            ReadU32(static_cast<uint8_t*>(pending.cmo) + m2off::kOffCmoRace, raceId);
            ReadU32(static_cast<uint8_t*>(pending.cmo) + m2off::kOffCmoGender, genderId);
            uint32_t fallbackRace = ClientModelRace(raceId);
            uint32_t retailRace = RetailModelRace(raceId);
            if (wxl_modern_m2::AlternateFormIdentity(pending.cmo,retailRace,genderId) &&
                (retailRace==52 || retailRace==70)) { fallbackRace=retailRace; genderId=0; }
            uint32_t inventoryType = 0;
            if (const auto display = index.displayRecords.find(pending.displayId);
                display != index.displayRecords.end())
                inventoryType = display->second.inventoryType;

            DetachModelSlot(pending.cmo, pending.modelSlot);
            if (!modelsOnly) DetachNativeSlotFallback(scene, pending.modelSlot);
            if (raceId == 28 && !IsWeaponInventoryType(inventoryType))
            {
                if (logAttempt)
                    WLOG_INFO("retail-equipment: murloc weapon-only suppressed display=%u inventory=%u",
                              pending.displayId, inventoryType);
                return true;
            }

            // Dragon shoulders and compatible waist models are attachments only.
            // Keep humanoid armor out of the body atlas in either preview or world.
            const bool dragon = retailRace == 52 || retailRace == 70;
            if (dragon && pending.modelSlot < kModelToEquipmentSlot.size() &&
                !DragonSlotVisible(kModelToEquipmentSlot[pending.modelSlot])) {
                dragonSlotClearOwner_ = pending.cmo;
                SafeClearCharacterSlot(pending.cmo, kModelToEquipmentSlot[pending.modelSlot] - 1);
                dragonSlotClearOwner_ = nullptr;
                return true;
            }
            // A live-model repair must not recompose the outfit and invalidate other children.
            const bool componentsApplied = modelsOnly || dragon || ReplayNativeComponents(pending, index);
            const std::vector<itemdisplay::ModelEntry>* entries =
                index.FindModels(pending.displayId);
            if (!entries || entries->empty())
            {
                if (logAttempt)
                    WLOG_INFO("retail-equipment: display=%u resolved without object models",
                              pending.displayId);
                return componentsApplied;
            }
            if (logAttempt)
                WLOG_INFO(
                    "retail-equipment: model family display=%u inventory=%u race=%u fallback=%u gender=%u",
                    pending.displayId, inventoryType, raceId, fallbackRace, genderId);

            AttachedSlot attached{pending.modelSlot, {}};
            pending.suppressedVariant.clear();
            bool suppressedCollection = false;
            uint32_t slotMatches = 0;
            uint32_t preferredMatches = 0;
            uint32_t pathsBuilt = 0;
            uint32_t modelsLoaded = 0;
            for (const itemdisplay::ModelEntry& entry : *entries)
            {
                if (entry.modelSlot != kMissing && entry.modelSlot != pending.modelSlot) continue;
                ++slotMatches;
                if (entry.attachId == kMissing ||
                    !IsPreferredModel(*entries, entry, retailRace, fallbackRace,
                                      genderId))
                    continue;
                ++preferredMatches;

                char modelPath[264]{};
                if (!BuildModelPath(entry, modelPath)) continue;
                ++pathsBuilt;
                const bool rootSkinned = IsRootSkinnedCollection(entry);
                if (rootSkinned &&
                    !wxl_modern_m2::ConfigBool("WXL_M2_COLLECTIONS", true))
                    continue;
                char filteredPath[264]{};
                char* loadPath = modelPath;
                if (rootSkinned &&
                    wxl::client::charmodel::PrepareRetailSkinPath(
                        modelPath, inventoryType,
                        filteredPath, std::size(filteredPath)))
                    loadPath = filteredPath;
                const char* collectionIdentity = loadPath;
                if (rootSkinned &&
                    !ClaimCollectionVariant(
                        pending, scene, collectionIdentity))
                {
                    suppressedCollection = true;
                    continue;
                }

                char retailHelmet[264]{};
                if(BuildRetailHelmetPath(pending.cmo,loadPath,retailHelmet))loadPath=retailHelmet;
                void* renderCtx = SafeGetRenderCtx(owner, loadPath);
                if (!renderCtx)
                {
                    pending.modelRetryRequired = true;
                    WLOG_WARN("retail-equipment: display=%u model load failed: %s",
                              pending.displayId, loadPath);
                    continue;
                }
                ++modelsLoaded;

                char texturePath[264]{};
                if (BuildTexturePath(entry, texturePath))
                {
                    void* texture = SafeLoadResource(texturePath);
                    if (texture) SafeBindTexture(renderCtx, 2, texture);
                    else pending.modelRetryRequired = true;
                }
                BindModelMaterials(renderCtx, pending.displayId, entry.modelIndex, index,
                                   retailRace, fallbackRace, genderId);

                AttachedSlot::Model model{
                    entry.attachId, renderCtx, scene, {},
                    rootSkinned, false, {}
                };
                if (rootSkinned)
                {
                    model.collectionVariant = collectionIdentity;
                    model.remap = BuildBoneRemap(renderCtx, scene);
                    ApplyCharacterRetargetPolicy(model.remap, pending.cmo);
                    if (!model.remap.count)
                    {
                        SafeReleaseRenderCtx(renderCtx);
                        pending.modelRetryRequired = true;
                        if (logAttempt)
                            WLOG_WARN("retail-equipment: deferred unremappable root display=%u model=%s",
                                      pending.displayId, loadPath);
                        continue;
                    }
                    CopyBonePalette(renderCtx, scene, model.remap);
                }

                if (SafeAttach(renderCtx, scene, entry.attachId, rootSkinned))
                {
                    attached.models.push_back(model);
                    if (modelsOnly)
                        WLOG_INFO("attachment-repair-detail: cmo=%p scene=%p child=%p display=%u slot=%u point=%u present=%u",
                            pending.cmo, scene, renderCtx, pending.displayId, pending.modelSlot, entry.attachId,
                            LiveAttachmentPresent(scene, renderCtx, entry.attachId) ? 1u : 0u);
                    if (logAttempt)
                        WLOG_INFO("retail-equipment: attached display=%u slot=%u point=%u root=%u race=%u gender=%u model=%s remap=%u key=%u name=%u inherited=%u sources=%u scale=%.3f",
                                  pending.displayId, pending.modelSlot, entry.attachId,
                                  rootSkinned ? 1u : 0u, entry.raceId, entry.genderId,
                                  loadPath, model.remap.count,
                                  model.remap.keyMatches,
                                  model.remap.nameMatches,
                                  model.remap.inheritedMatches,
                                  model.remap.uniqueSources,
                                  model.remap.geometryScale);
                }
                else
                {
                    pending.modelRetryRequired = true;
                    WLOG_WARN("retail-equipment: attach fault display=%u model=%s",
                              pending.displayId, modelPath);
                }
            }
            if (logAttempt)
                WLOG_INFO(
                    "retail-equipment: attach-summary display=%u slot=%u entries=%zu slotMatches=%u preferred=%u paths=%u loaded=%u attached=%zu components=%u suppressed=%u",
                    pending.displayId, pending.modelSlot, entries->size(),
                    slotMatches, preferredMatches, pathsBuilt, modelsLoaded,
                    attached.models.size(), componentsApplied ? 1u : 0u,
                    suppressedCollection ? 1u : 0u);
            if (!attached.models.empty())
            {
                attached_[pending.cmo].push_back(std::move(attached));
                rootBindingsDirty_ = true;
                // A glue/character-select model may have completed its initial geoset pass
                // before the asynchronous display graph and attachments became available.
                ApplyEquipmentGeometry(pending.cmo);
                return true;
            }
            return suppressedCollection || componentsApplied;
        }

        void OnItemSlotChange(const ev::ItemSlotChangeArgs& args)
        {
            if (replayingComponents_) return;
            uint32_t displayId = 0;
            const bool displayReadable = ReadU32(args.itemDataPtr, displayId);
            const uint32_t diagnostic = g_slotDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            const bool retailDisplay = displayReadable && displayId >= 100000;
            if (diagnostic < 64 || (ItemDetailLog() && retailDisplay))
                WLOG_INFO("retail-equipment: slot-event cmo=%p slot=%u data=%p readable=%u display=%u",
                          args.charModelObj, args.modelSlot, args.itemDataPtr,
                          displayReadable ? 1u : 0u, displayId);

            if (!args.charModelObj || args.modelSlot >= 11) return;
            if (!displayReadable || !displayId)
            {
                ForgetVariantSlotOwner(args.charModelObj, args.modelSlot);
                ForgetPending(args.charModelObj, args.modelSlot);
                DetachModelSlot(args.charModelObj, args.modelSlot);
                return;
            }
            if (NativeDisplayExists(displayId))
            {
                ForgetVariantSlotOwner(args.charModelObj, args.modelSlot);
                ForgetPending(args.charModelObj, args.modelSlot);
                DetachModelSlot(args.charModelObj, args.modelSlot);
                return;
            }

            RememberVariantSlotOwner(
                args.charModelObj, args.modelSlot, displayId);
            PendingSlot& pending = Remember(args.charModelObj, args.modelSlot, displayId);
            if (ItemDetailLog())
                WLOG_INFO("retail-equipment: requesting slot=%u display=%u",
                          args.modelSlot, displayId);
            itemdisplay::Request(displayId);
            if (const auto current = itemdisplay::Current();
                current && current->resolvedDisplays.contains(displayId))
            {
                // The client re-dispatches the equipped set when the selected
                // Glue model becomes the in-world player, but it can retain
                // the same CMO/scene address after clearing child
                // attachments. Treat every native slot dispatch as
                // authoritative and rebuild it immediately. Deferring this
                // transition leaves the world model and CharacterModelFrame
                // without the source attachment tree they clone.
                if (AttachResolved(pending, *current))
                {
                    MarkApplied(pending);
                }
            }
        }

        // No index rebuild, container writes or allocation in the palette callback.
        // Attachment churn elsewhere must not suspend remapping on existing actors.
        bool RemapCurrentBinding(void* renderCtx)
        {
            void* childModel = ReadPtr(static_cast<uint8_t*>(renderCtx) +
                                       m2off::kOffInstModel);
            if (!childModel) return false;
            for (bool exactContext : {true, false})
            {
                auto apply = [&](AttachedSlot::Model& model) {
                    if (!model.rootSkinned || !model.remap.count ||
                        model.remap.collectionModel != childModel ||
                        (exactContext && model.renderCtx != renderCtx))
                        return false;
                    void* ancestor = FindAncestorWithModel(
                        renderCtx, model.remap.characterModel);
                    if (!ancestor ||
                        !BoneRemapMatches(model.remap, renderCtx, ancestor))
                        return false;
                    return CopyBonePalette(renderCtx, ancestor, model.remap);
                };
                for (auto& [cmo, slots] : attached_)
                    for (AttachedSlot& slot : slots)
                        for (AttachedSlot::Model& model : slot.models)
                            if (apply(model)) return true;
                for (auto& [cmo, state] : customizationAttachments_)
                    for (AttachedSlot::Model& model : state.models)
                        if (apply(model)) return true;
            }
            return false;
        }

        void OnBuildBonePalette(const ev::BuildBonePaletteArgs& args)
        {
            if (!args.renderCtx) return;
            // Keep the crash-prevention rule: index rebuilding belongs to OnUpdate.
            // The current attachment records can still remap unaffected actors while
            // thumbnails dirty the shared acceleration index. This walk allocates
            // nothing and never consumes stale context/index entries.
            if (rootBindingsDirty_)
            {
                RemapCurrentBinding(args.renderCtx);
                return;
            }

            void* childModel = ReadPtr(
                static_cast<uint8_t*>(args.renderCtx) + m2off::kOffInstModel);
            if (const auto owner =
                    rootOwnerByContext_.find(args.renderCtx);
                owner != rootOwnerByContext_.end())
            {
                void* liveCharacter = ReadPtr(
                    static_cast<uint8_t*>(owner->second) +
                    m2off::kOffCmoSceneNode);
                AttachedSlot::Model* model =
                    FindRootBinding(owner->second, args.renderCtx);
                if (!liveCharacter || !model)
                {
                    rootBindingsDirty_ = true;
                    return;
                }

                // The native attachment parent is authoritative after the
                // model enters the scene. Glue and mounted character trees can
                // insert an intermediate context, so the scene pointer
                // captured by the slot event is not always the live palette
                // owner.
                void* poseSource = FindAncestorWithModel(
                    model->renderCtx, model->remap.characterModel);
                if (!poseSource)
                    poseSource = ReadPtr(
                        static_cast<uint8_t*>(model->renderCtx) +
                        m2off::kOffInstParent);
                if (!poseSource) poseSource = liveCharacter;
                if (!poseSource || poseSource == model->renderCtx) return;

                if (!BoneRemapMatches(
                        model->remap, model->renderCtx, poseSource))
                {
                    AttachedSlot::BoneRemap rebuilt =
                        BuildBoneRemap(model->renderCtx, poseSource);
                    if (!rebuilt.count) return;
                    ApplyCharacterRetargetPolicy(rebuilt, owner->second);
                    model->remap = rebuilt;
                    model->characterCtx = poseSource;
                    rootBindingsDirty_ = true;
                    WLOG_INFO(
                        "retail-equipment: rebuilt root pose child=%p character=%p remap=%u key=%u name=%u inherited=%u sources=%u scale=%.3f",
                        model->renderCtx, poseSource, model->remap.count,
                        model->remap.keyMatches, model->remap.nameMatches,
                        model->remap.inheritedMatches,
                        model->remap.uniqueSources,
                        model->remap.geometryScale);
                }
                CopyBonePalette(
                    model->renderCtx, poseSource, model->remap);
                return;
            }

            // CharacterModelFrame duplicates the scene tree without replaying
            // equipment events. Indexing by the shared collection model keeps
            // that compatibility path while avoiding a global attachment scan
            // for every unrelated M2 palette build.
            if (!childModel) return;
            const auto candidates = rootOwnersByModel_.find(childModel);
            if (candidates == rootOwnersByModel_.end()) return;
            for (void* cmo : candidates->second)
            {
                if (!ReadPtr(static_cast<uint8_t*>(cmo) +
                             m2off::kOffCmoSceneNode))
                    continue;
                AttachedSlot::Model* model =
                    FindRootBinding(cmo, nullptr, childModel);
                if (!model || !model->remap.characterModel) continue;
                void* cloneCharacter = FindAncestorWithModel(
                    args.renderCtx, model->remap.characterModel);
                if (!cloneCharacter) continue;
                if (CopyBonePalette(
                        args.renderCtx, cloneCharacter, model->remap) &&
                    !model->cloneRemapLogged)
                {
                    model->cloneRemapLogged = true;
                    WLOG_INFO(
                        "retail-equipment: remapped CharacterModelFrame clone child=%p character=%p",
                        args.renderCtx, cloneCharacter);
                }
                return;
            }
        }

        void OnItemDisplayApply(const ev::ItemDisplayApplyArgs& args)
        {
            if (replayingComponents_) return;
            if (!args.charModelObj || args.modelSlot >= 11 || !args.displayId) return;
            // Native DBC shoulders bypass pending_. Track authored pairs as well so a
            // late component pass cannot leave an incomplete outfit marked ready.
            if (!IsInWorld() && args.modelSlot == 1 && NativeShoulderPair(args.displayId))
            {
                glueShoulderOwner_ = args.charModelObj;
                glueShoulderRoot_ = ReadPtr(static_cast<uint8_t*>(args.charModelObj) + m2off::kOffCmoSceneNode);
                glueShoulderDisplay_ = args.displayId;
                glueShoulderChecks_ = 3;
                glueShoulderDue_ = GetTickCount() + 500;
            }
            if (NativeDisplayExists(args.displayId)) return;

            RememberVariantSlotOwner(
                args.charModelObj, args.modelSlot, args.displayId);
            const uint32_t diagnostic =
                g_displayApplyDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed);
            if (diagnostic < 64 || ItemDetailLog())
                WLOG_INFO("retail-equipment: display-apply cmo=%p slot=%u display=%u",
                          args.charModelObj, args.modelSlot, args.displayId);
            PendingSlot& pending = Remember(
                args.charModelObj, args.modelSlot, args.displayId, args.postFlag);
            itemdisplay::Request(args.displayId);
            if (const auto current = itemdisplay::Current();
                current && current->resolvedDisplays.contains(args.displayId) &&
                !pending.applied && !GlueGearHiddenFor(pending.cmo))
            {
                if (AttachResolved(pending, *current))
                    MarkApplied(pending);
            }
        }

        void OnItemSlotClear(const ev::ItemSlotClearArgs& args)
        {
            if (!args.charModelObj || args.equipSlotWow >= kEquipToModelSlot.size()) return;
            const uint32_t modelSlot = kEquipToModelSlot[args.equipSlotWow];
            if (modelSlot == kMissing) return;
            if (args.charModelObj == dragonReplayOwner_ &&
                DragonEquipmentSlot(kModelToEquipmentSlot[modelSlot]))
                dragonReplayDisplays_[modelSlot] = kMissing;
            if (!replayingComponents_ && !preservePendingSlotClear_ &&
                args.charModelObj == glueShoulderOwner_ && modelSlot == 1)
                glueShoulderChecks_ = 0;
            if ((preservePendingSlotClear_ && args.charModelObj == latestGlueCmo_) ||
                args.charModelObj == dragonSlotClearOwner_)
            {
                DetachModelSlot(args.charModelObj, modelSlot);
                return;
            }
            ForgetVariantSlotOwner(args.charModelObj, modelSlot);
            ForgetPending(args.charModelObj, modelSlot);
            DetachModelSlot(args.charModelObj, modelSlot);
        }

        void OnUpdate(const ev::UpdateArgs& args)
        {
            RepairGlueShoulders();
            ReconcileDragonEquipment();
            ReconcileActivePlayerVariants(
                wxl::client::wxlwow::VariantGeneration());
            // Customization children do not depend on ItemDisplayInfo. Consume them before the
            // item-display snapshot's early return so character creation remains functional while
            // wxl-db2 is still resolving wardrobe data (or when no retail item is equipped).
            ProcessCustomizationAttachments(args.timeMs);
            const auto current = itemdisplay::Current();
            if (!current)
            {
                RepairPendingRootRemaps();
                EnsureRootBindings();
                return;
            }
            uint32_t attachmentWork = 0;
            for (size_t i = 0; i < pending_.size();)
            {
                PendingSlot& pending = pending_[i];
                const uint32_t now = GetTickCount();
                void* liveScene = ReadPtr(
                    static_cast<uint8_t*>(pending.cmo) + m2off::kOffCmoSceneNode);
                if (!liveScene && !pending.sceneIdentity &&
                    now - pending.createdAtMs >=
                        kPendingSceneTimeoutMs)
                {
                    if (ItemDetailLog())
                        WLOG_INFO(
                            "retail-equipment: pruned never-materialized cmo=%p slot=%u display=%u",
                            pending.cmo, pending.modelSlot,
                            pending.displayId);
                    pending_.erase(
                        pending_.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                if (!liveScene && pending.sceneIdentity)
                {
                    if (!pending.sceneMissingSinceMs)
                    {
                        attached_.erase(pending.cmo);
                        rootBindingsDirty_ = true;
                        for (PendingSlot& dormant : pending_)
                        {
                            if (dormant.cmo != pending.cmo) continue;
                            dormant.sceneMissingSinceMs = now;
                            dormant.applied = false;
                            dormant.modelRetryRequired = false;
                            dormant.componentRetryAtMs = 0;
                            dormant.componentRetries = 0;
                            dormant.suppressedVariant.clear();
                        }
                        WLOG_INFO("retail-equipment: retained dormant cmo=%p scene=%p",
                                  pending.cmo, pending.sceneIdentity);
                    }
                    if (now - pending.sceneMissingSinceMs >=
                        kPendingSceneTimeoutMs)
                    {
                        const void* retired = pending.cmo;
                        std::erase_if(pending_, [&](const PendingSlot& slot) {
                            return slot.cmo == retired;
                        });
                        WLOG_INFO("retail-equipment: expired dormant cmo=%p",
                                  retired);
                        i = 0;
                        continue;
                    }
                    ++i;
                    continue;
                }
                if (liveScene && pending.sceneIdentity &&
                    liveScene != pending.sceneIdentity)
                {
                    attached_.erase(pending.cmo);
                    rootBindingsDirty_ = true;
                    for (PendingSlot& recycled : pending_)
                    {
                        if (recycled.cmo != pending.cmo) continue;
                        recycled.sceneIdentity = liveScene;
                        recycled.createdAtMs = now;
                        recycled.sceneMissingSinceMs = 0;
                        recycled.applied = false;
                        recycled.modelRetryRequired = false;
                        recycled.componentRetryAtMs = 0;
                        recycled.componentRetries = 0;
                        recycled.suppressedVariant.clear();
                    }
                    WLOG_INFO("retail-equipment: rebound recycled cmo=%p scene=%p; replaying wardrobe",
                              pending.cmo, liveScene);
                }
                if (!pending.sceneIdentity) pending.sceneIdentity = liveScene;
                pending.sceneMissingSinceMs = 0;
                if (GlueGearHiddenFor(pending.cmo))
                {
                    DetachModelSlot(pending.cmo, pending.modelSlot);
                    pending.applied = false;
                    pending.modelRetryRequired = false;
                    pending.componentRetryAtMs = 0;
                    pending.componentRetries = 0;
                    pending.suppressedVariant.clear();
                    ++i;
                    continue;
                }
                if (NativeDisplayExists(pending.displayId))
                {
                    DetachModelSlot(pending.cmo, pending.modelSlot);
                    pending_.erase(pending_.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                bool repairModelsOnly = false;
                // A later native composition can unlink an already recorded child. Do not
                // let cached "applied" state mask that loss on warm race switches.
                if (!IsInWorld() && pending.cmo == latestGlueCmo_ && pending.applied &&
                    now - pending.createdAtMs >= 400 && now - pending.createdAtMs <= 15000 &&
                    pending.liveRepairAttempts < 3 && RetryDue(now, pending.liveRepairAtMs) &&
                    SlotHasMissingLiveModels(pending))
                {
                    repairModelsOnly = true;
                    ++pending.liveRepairAttempts;
                    pending.liveRepairAtMs = now + 1000;
                    pending.applied = false;
                    pending.modelRetryRequired = true;
                    pending.componentRetries = 0;
                    pending.componentRetryAtMs = 0;
                    WLOG_INFO("retail-equipment: live attachment lost display=%u slot=%u repair=%u",
                        pending.displayId, pending.modelSlot, pending.liveRepairAttempts);
                }
                if (!current->resolvedDisplays.contains(pending.displayId) ||
                    pending.applied)
                {
                    if (pending.applied && pending.componentRetries)
                    {
                        if (!pending.componentRetryAtMs)
                        {
                            pending.componentRetryAtMs =
                                args.timeMs + RetryDelay(pending);
                        }
                        else if (RetryDue(args.timeMs, pending.componentRetryAtMs))
                        {
                            if (attachmentWork >=
                                kMaxAttachmentWorkPerFrame)
                            {
                                ++i;
                                continue;
                            }
                            ++attachmentWork;
                            const bool retryModels =
                                pending.modelRetryRequired;
                            // ApplyDisplay can clear or replace native
                            // attachment points while recomposing body layers.
                            // Always rebuild our owned object models after the
                            // delayed component pass; replaying only the body
                            // compositor is what left one shoulder/collection
                            // side missing after first load.
                            const bool replayed =
                                AttachResolved(pending, *current, pending.liveRepairAttempts > 0);
                            --pending.componentRetries;
                            const bool complete =
                                replayed && !pending.modelRetryRequired;
                            if (complete)
                            {
                                pending.componentRetries = 0;
                                pending.componentRetryAtMs = 0;
                            }
                            else
                            {
                                pending.componentRetryAtMs =
                                    pending.componentRetries
                                        ? args.timeMs + RetryDelay(pending)
                                        : 0u;
                            }
                            if (complete)
                            {
                                if (ItemDetailLog())
                                    WLOG_INFO(
                                        "retail-equipment: delayed %s replay display=%u slot=%u remaining=%u",
                                        retryModels ? "model" : "component",
                                        pending.displayId, pending.modelSlot,
                                        static_cast<unsigned>(pending.componentRetries));
                            }
                            else if (!pending.componentRetries)
                            {
                                WLOG_WARN(
                                    "retail-equipment: delayed %s replay exhausted display=%u slot=%u",
                                    retryModels ? "model" : "component",
                                    pending.displayId, pending.modelSlot);
                            }
                        }
                    }
                    ++i;
                    continue;
                }
                if (attachmentWork >= kMaxAttachmentWorkPerFrame)
                {
                    ++i;
                    continue;
                }
                // Never charge the small attachment budget for a CMO whose
                // scene tree has not materialized yet. Dense object updates
                // commonly publish fields before their render owner exists.
                if (!liveScene ||
                    !ReadPtr(static_cast<uint8_t*>(liveScene) +
                             m2off::kOffSceneNodeOwner))
                {
                    ++i;
                    continue;
                }
                ++attachmentWork;
                AttachResolved(pending, *current, repairModelsOnly || pending.liveRepairAttempts > 0);
                // The graph itself is resolved even when this particular
                // attempt produced no component/model. Mark it so a failure
                // enters the bounded delayed path instead of monopolizing the
                // first two work slots every frame.
                MarkApplied(pending, args.timeMs);
                ++i;
            }
            if (glueGearSheetRebuildPending_ && glueGearVisible_ && GlueEquipmentReady())
            {
                RequestCharacterAppearanceRebuild(wxl_modern_m2::CustomizeComponent());
                glueGearSheetRebuildPending_ = false;
                WLOG_INFO("retail-equipment: rebuilt Glue body sheet after all shown gear components replayed");
            }
            RepairPendingRootRemaps();
            EnsureRootBindings();
        }
    };

    std::unique_ptr<RetailEquipment> g_retailEquipment;

    int __cdecl LuaGlueEquipmentReady(void* state)
    {
        const bool ready = g_retailEquipment &&
            g_retailEquipment->GlueEquipmentReady();
        wxl::game::Native<luaoff::LuaPushBooleanFn>(luaoff::kLuaPushBoolean)(
            state, ready ? 1 : 0);
        return 1;
    }

    int __cdecl LuaGlueGearVisible(void* state)
    {
        const double raw = wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber)(state, 1);
        const bool changed = std::isfinite(raw) && g_retailEquipment &&
            g_retailEquipment->SetGlueGearVisible(raw != 0.0);
        wxl::game::Native<luaoff::LuaPushBooleanFn>(luaoff::kLuaPushBoolean)(
            state, changed ? 1 : 0);
        return 1;
    }

    int __cdecl LuaDragonGearVisible(void* state)
    {
        const auto number = wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber);
        const double shoulders = number(state, 1), waist = number(state, 2);
        if (g_retailEquipment && std::isfinite(shoulders) && std::isfinite(waist))
            g_retailEquipment->SetDragonGearVisible(shoulders != 0, waist != 0);
        return 0;
    }

    bool InstallRetailEquipment()
    {
        bool ok = true;
        g_npcTextureTrace = wxl_modern_m2::ConfigBool("WXL_M2_NPC_TEXTURE_TRACE", false);
        if (g_npcTextureTrace) {
            g_npcTextureTrace = wxl_modern_m2::HookAttachByName(
                "Gx.TextureCreate", &TraceTextureCreate, &g_originalTraceTextureCreate);
            ok &= g_npcTextureTrace;
        }
        WLOG_INFO("npc-texture-bind-v1: enabled=%u maxBindings=2048", g_npcTextureTrace);

        ok &= wxl_modern_m2::HookAttach(
            "RetailEquipment.CreateSceneModel", m2off::kCreateSceneModel,
            &CreateSceneModel, &g_originalCreateSceneModel);
        ok &= wxl_modern_m2::HookAttach(
            "RetailEquipment.BindTexSlot", m2off::kBindTexSlot,
            &BindTexSlot, &g_originalBindTexSlot);
        ok &= wxl_modern_m2::HookAttach(
            "RetailEquipment.CharacterGeosRenderPrep",
            private_m2::kCharacterGeosRenderPrep, &CharacterGeosRenderPrep,
            // Lower priority is the OUTER hook: its post-original work runs last.
            // Customization uses default priority and must finish showing its recipe
            // before equipment hides hair/ears beneath the selected helmet.
            &g_originalCharacterGeosRenderPrep, WXL_HOOK_DEFAULT_PRIORITY - 1);
        g_retailEquipment = std::make_unique<RetailEquipment>();
        if (const WXL_FrameScriptApi* frameScript = wxl_modern_m2::FrameScript())
        {
            ok &= frameScript->RegisterFunction(
                "_WXL_M2_DRAGON_GEAR_VISIBLE", &LuaDragonGearVisible) != 0;
            ok &= frameScript->RegisterFunction(
                "_WXL_CC_EQUIPMENT_READY", &LuaGlueEquipmentReady) != 0;
            ok &= frameScript->RegisterFunction(
                "_WXL_M2_CUSTOMIZATION_GEAR_VISIBLE", &LuaGlueGearVisible) != 0;
        }
        return ok;
    }
}

bool wxl_modern_m2::InstallRetailEquipment()
{
    return ::InstallRetailEquipment();
}

void wxl_modern_m2::SyncGilneanEquipment(void* primary, void* alternate)
{
    if (g_retailEquipment) g_retailEquipment->SyncGilneanEquipment(primary,alternate);
}
void wxl_modern_m2::ReleaseGilneanEquipment(void* alternate)
{
    if (g_retailEquipment) g_retailEquipment->ReleaseGilneanEquipment(alternate);
}
bool wxl_modern_m2::GilneanGearVisible()
{
    return !g_retailEquipment || g_retailEquipment->GearVisible();
}

void wxl_modern_m2::QueueCustomizationAttachments(
    void* cmo, void* rootInstance, uint32_t chrModel, uint32_t generation,
    const CustomizationAttachmentSpec* attachments, uint32_t attachmentCount)
{
    if (g_retailEquipment)
        g_retailEquipment->QueueCustomizationAttachments(
            cmo, rootInstance, chrModel, generation,
            attachments, attachmentCount);
}

void wxl_modern_m2::ResetCustomizationAttachments() noexcept
{
    if (g_retailEquipment)
        g_retailEquipment->ResetCustomizationAttachments();
}

bool wxl_modern_m2::SuppressGlueRetailDisplay(void* cmo, uint32_t displayId)
{
    return g_retailEquipment &&
           g_retailEquipment->SuppressGlueRetailDisplay(cmo, displayId);
}

void wxl_modern_m2::ReleaseCustomizationAttachments(void* cmo) noexcept
{
    if (g_retailEquipment) g_retailEquipment->ReleaseCustomizationAttachments(cmo);
}


bool wxl_modern_m2::CustomizationAttachmentsReady(void* cmo) noexcept
{
    return g_retailEquipment && g_retailEquipment->CustomizationAttachmentsReady(cmo);
}

void wxl_modern_m2::ForgetCharacterEquipment(void* cmo) noexcept
{
    if (g_retailEquipment) g_retailEquipment->ForgetCharacterEquipment(cmo);
}

void wxl_modern_m2::PrepareNativeEquipmentSlot(void* cmo, uint32_t modelSlot)
{
    if (g_retailEquipment) g_retailEquipment->PrepareNativeEquipmentSlot(cmo, modelSlot);
}

void* wxl_modern_m2::SetEquipmentModelOwner(void* cmo) noexcept
{
    void* previous=g_equipmentModelOwner;g_equipmentModelOwner=cmo;return previous;
}

void wxl_modern_m2::RefreshEquipmentGeometry(void* cmo)
{
    ApplyEquipmentGeometry(cmo);
}
